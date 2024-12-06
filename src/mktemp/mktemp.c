#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

enum {
    BX_MKTEMP_OPT_TMPDIR = 256,
};

struct bx_mktemp_options {
    const char* progname;
    bool create_directory;
    bool dry_run;
    bool quiet;
    bool tmpdir_specified;
    const char* tmpdir_arg;
    bool show_help;
    bool show_version;
};

static const char* bx_mktemp_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mktemp";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_mktemp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [TEMPLATE]\n", progname);
    fprintf(stream, "Create a temporary file or directory and print its name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --directory     create a directory, not a file\n");
    fprintf(stream, "  -u, --dry-run       print a name without keeping the created path\n");
    fprintf(stream, "  -q, --quiet         suppress diagnostics for creation failures\n");
    fprintf(stream, "  -p DIR              interpret TEMPLATE relative to DIR\n");
    fprintf(stream, "      --tmpdir[=DIR]  like -p; if DIR is omitted use $TMPDIR or /tmp\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "If TEMPLATE is omitted, mktemp uses tmp.XXXXXX with --tmpdir implied.\n");
    fprintf(stream, "TEMPLATE must end with at least 6 'X' characters.\n");
}

static void bx_mktemp_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_mktemp_parse_options(int argc, char** argv, struct bx_mktemp_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"directory", no_argument, NULL, 'd'},
        {"dry-run", no_argument, NULL, 'u'},
        {"quiet", no_argument, NULL, 'q'},
        {"tmpdir", optional_argument, NULL, BX_MKTEMP_OPT_TMPDIR},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mktemp_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+duqp:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->create_directory = true;
                break;
            case 'u':
                options->dry_run = true;
                break;
            case 'q':
                options->quiet = true;
                break;
            case 'p':
                options->tmpdir_specified = true;
                options->tmpdir_arg = optarg;
                break;
            case BX_MKTEMP_OPT_TMPDIR:
                options->tmpdir_specified = true;
                options->tmpdir_arg = optarg;
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

static const char* bx_mktemp_default_tmpdir(void) {
    const char* tmpdir_env = getenv("TMPDIR");
    if (tmpdir_env != NULL && tmpdir_env[0] != '\0') {
        return tmpdir_env;
    }

    return "/tmp";
}

static char* bx_mktemp_join_path(const char* directory, const char* template_text) {
    if (directory == NULL || directory[0] == '\0') {
        return xstrdup(template_text);
    }

    size_t directory_len = strlen(directory);
    size_t template_len = strlen(template_text);
    bool need_slash = directory[directory_len - 1] != '/';

    size_t joined_len = directory_len + (need_slash ? 1u : 0u) + template_len + 1u;
    char* joined = xmalloc(joined_len);
    memcpy(joined, directory, directory_len);

    size_t offset = directory_len;
    if (need_slash) {
        joined[offset++] = '/';
    }

    memcpy(joined + offset, template_text, template_len + 1u);
    return joined;
}

static bool bx_mktemp_template_valid(const char* template_text) {
    size_t template_len = strlen(template_text);
    if (template_len < 6) {
        return false;
    }

    for (size_t i = template_len - 6; i < template_len; i++) {
        if (template_text[i] != 'X') {
            return false;
        }
    }

    return true;
}

static bool bx_mktemp_create_path(const struct bx_mktemp_options* options, char* path_out) {
    if (options->create_directory) {
        if (mkdtemp(path_out) == NULL) {
            return false;
        }

        if (options->dry_run && rmdir(path_out) != 0) {
            return false;
        }

        return true;
    }

    int fd = mkstemp(path_out);
    if (fd < 0) {
        return false;
    }

    if (close(fd) != 0) {
        int saved_errno = errno;
        (void)unlink(path_out);
        errno = saved_errno;
        return false;
    }

    if (options->dry_run && unlink(path_out) != 0) {
        return false;
    }

    return true;
}

int bx_mktemp_main(int argc, char** argv) {
    struct bx_mktemp_options options;
    struct bx_diag_ctx diag = {
        .progname = "mktemp",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mktemp_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mktemp_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_mktemp_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count > 1) {
        bx_diag(&diag, "extra operand '%s'", argv[first_operand + 1]);
        return diag.exit_status;
    }

    const char* template_text = (operand_count == 1) ? argv[first_operand] : "tmp.XXXXXX";
    bool use_tmpdir = options.tmpdir_specified || operand_count == 0;
    const char* tmpdir = NULL;
    if (use_tmpdir) {
        tmpdir = (options.tmpdir_arg != NULL) ? options.tmpdir_arg : bx_mktemp_default_tmpdir();
    }

    if (use_tmpdir && template_text[0] == '/') {
        bx_diag(&diag, "template must not be absolute with --tmpdir: '%s'", template_text);
        return diag.exit_status;
    }

    char* full_template = use_tmpdir ? bx_mktemp_join_path(tmpdir, template_text) : xstrdup(template_text);
    if (!bx_mktemp_template_valid(full_template)) {
        bx_diag(&diag, "template must end with at least 6 'X' characters: '%s'", template_text);
        free(full_template);
        return diag.exit_status;
    }

    if (!bx_mktemp_create_path(&options, full_template)) {
        if (!options.quiet) {
            bx_perror_path(&diag, full_template);
        }
        else {
            diag.exit_status = 1;
        }
        free(full_template);
        return diag.exit_status;
    }

    printf("%s\n", full_template);
    free(full_template);
    return 0;
}
