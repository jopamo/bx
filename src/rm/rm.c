#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "common/path_ops.h"
#include "common/prompt_ops.h"
#include "common/remove_ops.h"
#include "diag.h"
#include "libbx.h"

enum bx_rm_interactive_mode {
    BX_RM_INTERACTIVE_NEVER = 0,
    BX_RM_INTERACTIVE_ONCE,
    BX_RM_INTERACTIVE_ALWAYS,
};

enum {
    BX_RM_OPT_INTERACTIVE = 256,
    BX_RM_OPT_NO_PRESERVE_ROOT,
    BX_RM_OPT_PRESERVE_ROOT,
    BX_RM_OPT_ONE_FILE_SYSTEM,
};

struct bx_rm_options {
    const char* progname;
    bool remove_empty_dirs;
    bool force;
    bool recursive;
    bool preserve_root;
    bool one_file_system;
    enum bx_rm_interactive_mode interactive_mode;
    bool show_help;
    bool show_version;
};

static const char* bx_rm_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "rm";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_rm_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Remove (unlink) the FILE(s).\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --dir        remove empty directories\n");
    fprintf(stream, "  -f, --force      ignore nonexistent files and arguments\n");
    fprintf(stream, "  -i               prompt before every removal\n");
    fprintf(stream, "  -I               prompt once before removing many files or recursively\n");
    fprintf(stream, "      --interactive[=WHEN]  prompt according to WHEN: never, once, or always\n");
    fprintf(stream, "      --no-preserve-root    do not treat '/' specially\n");
    fprintf(stream, "      --preserve-root       do not remove '/' (default)\n");
    fprintf(stream, "  -r, -R, --recursive  remove directories and their contents recursively\n");
    fprintf(stream, "  -x, --one-file-system  when removing a hierarchy recursively, skip directories on different file systems\n");
    fprintf(stream, "      --help       display this help and exit\n");
    fprintf(stream, "      --version    output version information and exit\n");
}

static void bx_rm_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_rm_parse_interactive_mode(const char* value, enum bx_rm_interactive_mode* mode_out) {
    if (value == NULL || strcmp(value, "always") == 0) {
        *mode_out = BX_RM_INTERACTIVE_ALWAYS;
        return true;
    }
    if (strcmp(value, "never") == 0) {
        *mode_out = BX_RM_INTERACTIVE_NEVER;
        return true;
    }
    if (strcmp(value, "once") == 0) {
        *mode_out = BX_RM_INTERACTIVE_ONCE;
        return true;
    }
    return false;
}

static bool bx_rm_parse_options(int argc, char** argv, struct bx_rm_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"dir", no_argument, NULL, 'd'},
        {"force", no_argument, NULL, 'f'},
        {"interactive", optional_argument, NULL, BX_RM_OPT_INTERACTIVE},
        {"no-preserve-root", no_argument, NULL, BX_RM_OPT_NO_PRESERVE_ROOT},
        {"one-file-system", no_argument, NULL, 'x'},
        {"preserve-root", no_argument, NULL, BX_RM_OPT_PRESERVE_ROOT},
        {"recursive", no_argument, NULL, 'r'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_rm_progname((argc > 0) ? argv[0] : NULL);
    options->preserve_root = true;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+dfiIRrx", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->remove_empty_dirs = true;
                break;
            case 'f':
                options->force = true;
                options->interactive_mode = BX_RM_INTERACTIVE_NEVER;
                break;
            case 'i':
                options->force = false;
                options->interactive_mode = BX_RM_INTERACTIVE_ALWAYS;
                break;
            case 'I':
                options->force = false;
                options->interactive_mode = BX_RM_INTERACTIVE_ONCE;
                break;
            case 'R':
            case 'r':
                options->recursive = true;
                break;
            case 'x':
                options->one_file_system = true;
                break;
            case BX_RM_OPT_INTERACTIVE:
                if (!bx_rm_parse_interactive_mode(optarg, &options->interactive_mode)) {
                    bx_diag(diag, "invalid argument '%s' for '--interactive'", optarg);
                    return false;
                }
                if (options->interactive_mode != BX_RM_INTERACTIVE_NEVER) {
                    options->force = false;
                }
                break;
            case BX_RM_OPT_NO_PRESERVE_ROOT:
                options->preserve_root = false;
                break;
            case BX_RM_OPT_PRESERVE_ROOT:
                options->preserve_root = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static bool bx_rm_operand_is_dot_or_dotdot(const char* path) {
    char* base = bx_path_basename_dup(path);
    bool is_dot = bx_path_is_dot_or_dotdot(base);
    free(base);
    return is_dot;
}

static bool bx_rm_operand_is_root_directory(const struct stat* path_st, struct bx_diag_ctx* diag, bool* is_root_out) {
    struct stat root_st;
    if (stat("/", &root_st) != 0) {
        bx_perror_path(diag, "/");
        return false;
    }

    *is_root_out = (path_st->st_dev == root_st.st_dev && path_st->st_ino == root_st.st_ino);
    return true;
}

static bool bx_rm_prompt_remove(const struct bx_rm_options* options, const char* path) {
    size_t prompt_len = strlen(options->progname) + strlen(path) + sizeof(": remove ''? ");
    char* prompt = xmalloc(prompt_len);

    snprintf(prompt, prompt_len, "%s: remove '%s'? ", options->progname, path);
    bool confirmed = bx_prompt_confirm(prompt);
    free(prompt);
    return confirmed;
}

static bool bx_rm_should_prompt_once(const struct bx_rm_options* options, int operand_count) {
    if (options->interactive_mode != BX_RM_INTERACTIVE_ONCE) {
        return false;
    }
    return options->recursive || operand_count > 3;
}

static bool bx_rm_prompt_once(const struct bx_rm_options* options, int operand_count) {
    const char* noun = (operand_count == 1) ? "argument" : "arguments";
    const char* recursive_suffix = options->recursive ? " recursively" : "";
    size_t prompt_len = strlen(options->progname) + strlen(noun) + strlen(recursive_suffix) + 48u;
    char* prompt = xmalloc(prompt_len);

    snprintf(prompt, prompt_len, "%s: remove %d %s%s? ", options->progname, operand_count, noun, recursive_suffix);
    bool confirmed = bx_prompt_confirm(prompt);
    free(prompt);
    return confirmed;
}

static bool bx_rm_remove_operand(const char* path, const struct bx_rm_options* options, struct bx_diag_ctx* diag) {
    struct stat st;
    bool is_root = false;

    if (bx_rm_operand_is_dot_or_dotdot(path)) {
        bx_diag(diag, "refusing to remove '.' or '..' directory: skipping '%s'", path);
        return false;
    }

    if (lstat(path, &st) != 0) {
        if (errno == ENOENT && options->force) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }

    if (options->recursive && options->preserve_root && S_ISDIR(st.st_mode)) {
        if (!bx_rm_operand_is_root_directory(&st, diag, &is_root)) {
            return false;
        }
        if (is_root) {
            bx_diag(diag, "refusing to remove '/' recursively");
            return false;
        }
    }

    if (S_ISDIR(st.st_mode)) {
        if (options->recursive) {
            if (options->interactive_mode == BX_RM_INTERACTIVE_ALWAYS && !bx_rm_prompt_remove(options, path)) {
                return true;
            }
            if (options->one_file_system) {
                return bx_remove_recursive_one_file_system(path, st.st_dev, diag);
            }
            return bx_remove_recursive(path, diag);
        }

        if (options->remove_empty_dirs) {
            if (options->interactive_mode == BX_RM_INTERACTIVE_ALWAYS && !bx_rm_prompt_remove(options, path)) {
                return true;
            }
            if (rmdir(path) == 0) {
                return true;
            }

            if (errno == ENOENT && options->force) {
                return true;
            }
            bx_perror_path(diag, path);
            return false;
        }

        errno = EISDIR;
        bx_perror_path(diag, path);
        return false;
    }

    if (options->interactive_mode == BX_RM_INTERACTIVE_ALWAYS && !bx_rm_prompt_remove(options, path)) {
        return true;
    }

    if (unlink(path) == 0) {
        return true;
    }

    if (errno == ENOENT && options->force) {
        return true;
    }
    bx_perror_path(diag, path);
    return false;
}

int bx_rm_main(int argc, char** argv) {
    struct bx_rm_options options;
    struct bx_diag_ctx diag = {
        .progname = "rm",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_rm_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_rm_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_rm_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        if (options.force) {
            return 0;
        }
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    if (bx_rm_should_prompt_once(&options, operand_count) && !bx_rm_prompt_once(&options, operand_count)) {
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        (void)bx_rm_remove_operand(argv[i], &options, &diag);
    }

    return diag.exit_status;
}
