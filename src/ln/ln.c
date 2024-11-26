#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "diag.h"
#include "libbx.h"

struct bx_ln_options {
    const char* progname;
    bool symbolic;
    bool force;
    bool no_dereference;
    bool no_target_directory;
    bool verbose;
    bool follow_symlinks;
    const char* target_directory;
    bool show_help;
    bool show_version;
};

static const char* bx_ln_progname(const char* argv0) {
    const char* base = strrchr(argv0, '/');
    return base ? base + 1 : argv0;
}

static void bx_ln_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... TARGET [LINK_NAME]\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... TARGET\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... TARGET... DIRECTORY\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -t DIRECTORY TARGET...\n", progname);
    fprintf(stream, "Create a link to TARGET with the name LINK_NAME.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --force                remove existing destination files\n");
    fprintf(stream, "  -L, --logical              dereference TARGETs that are symbolic links\n");
    fprintf(stream, "  -n, --no-dereference       treat LINK_NAME as a normal file if it is a symbolic link\n");
    fprintf(stream, "  -P, --physical             make hard links directly to symbolic links (default)\n");
    fprintf(stream, "  -s, --symbolic             make symbolic links instead of hard links\n");
    fprintf(stream, "  -t, --target-directory=DIR specify the directory for all links\n");
    fprintf(stream, "  -T, --no-target-directory  treat LINK_NAME as a normal file always\n");
    fprintf(stream, "  -v, --verbose              print name of each linked file\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

static void bx_ln_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_ln_parse_options(int argc, char** argv, struct bx_ln_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"force", no_argument, NULL, 'f'},
        {"logical", no_argument, NULL, 'L'},
        {"no-dereference", no_argument, NULL, 'n'},
        {"physical", no_argument, NULL, 'P'},
        {"symbolic", no_argument, NULL, 's'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };
    char short_opts[] = "+:fLnPst:Tv";

    memset(options, 0, sizeof(*options));
    options->progname = bx_ln_progname(argv[0]);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, short_opts, long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'f':
                options->force = true;
                break;
            case 'L':
                options->follow_symlinks = true;
                break;
            case 'n':
                options->no_dereference = true;
                break;
            case 'P':
                options->follow_symlinks = false;
                break;
            case 's':
                options->symbolic = true;
                break;
            case 't':
                options->target_directory = optarg;
                break;
            case 'T':
                options->no_target_directory = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                bx_diag(diag, "option requires an argument -- '%c'", optopt);
                return false;
            case '?':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
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

    if (options->target_directory != NULL && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory and --no-target-directory");
        return false;
    }

    *first_operand = optind;
    return true;
}

static bool bx_ln_path_is_directory(const char* path, bool follow_symlinks) {
    struct stat st;
    int rc = follow_symlinks ? stat(path, &st) : lstat(path, &st);
    return rc == 0 && S_ISDIR(st.st_mode);
}

static bool bx_ln_parent_dir_stat(const char* path, struct stat* parent_stat_out) {
    char* stripped = bx_path_strip_trailing_slashes_dup(path);
    char* parent_path = NULL;
    char* slash = strrchr(stripped, '/');
    bool ok = false;

    if (slash == NULL) {
        parent_path = xstrdup(".");
    }
    else if (slash == stripped) {
        parent_path = xstrdup("/");
    }
    else {
        size_t parent_len = (size_t)(slash - stripped);
        parent_path = xmalloc(parent_len + 1u);
        memcpy(parent_path, stripped, parent_len);
        parent_path[parent_len] = '\0';
    }

    ok = stat(parent_path, parent_stat_out) == 0;
    free(parent_path);
    free(stripped);
    return ok;
}

static bool bx_ln_paths_name_same_directory_entry(const char* left_path, const char* right_path) {
    bool same_entry = true;
    char* left_base = bx_path_basename_dup(left_path);
    char* right_base = bx_path_basename_dup(right_path);

    if (strcmp(left_base, right_base) != 0) {
        same_entry = false;
        goto out;
    }

    struct stat left_parent_stat;
    struct stat right_parent_stat;
    if (!bx_ln_parent_dir_stat(left_path, &left_parent_stat) || !bx_ln_parent_dir_stat(right_path, &right_parent_stat)) {
        goto out;
    }

    same_entry = bx_same_file(&left_parent_stat, &right_parent_stat);

out:
    free(right_base);
    free(left_base);
    return same_entry;
}

static bool bx_ln_hard_link_already_exists(const char* source_path, const char* destination_path, bool follow_symlinks) {
    struct stat src_stat;
    struct stat dest_stat;

    int src_rc = follow_symlinks ? stat(source_path, &src_stat) : lstat(source_path, &src_stat);
    if (src_rc != 0) {
        return false;
    }
    if (lstat(destination_path, &dest_stat) != 0) {
        return false;
    }
    return bx_same_file(&src_stat, &dest_stat);
}

static bool bx_ln_remove_destination_for_force(const char* destination_path, struct bx_diag_ctx* diag) {
    struct stat dest_lstat;

    if (lstat(destination_path, &dest_lstat) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "cannot remove '%s': %s", destination_path, strerror(errno));
        return false;
    }

    if (S_ISDIR(dest_lstat.st_mode)) {
        bx_diag(diag, "%s: cannot overwrite directory", destination_path);
        return false;
    }

    if (unlink(destination_path) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "cannot remove '%s': %s", destination_path, strerror(errno));
        return false;
    }

    return true;
}

static bool bx_ln_create_link_once(const struct bx_ln_options* options, const char* source_path, const char* destination_path, int* err_out) {
    int rc;
    if (options->symbolic) {
        rc = symlink(source_path, destination_path);
    }
    else {
        int flags = options->follow_symlinks ? AT_SYMLINK_FOLLOW : 0;
        rc = linkat(AT_FDCWD, source_path, AT_FDCWD, destination_path, flags);
    }

    if (rc == 0) {
        return true;
    }

    *err_out = errno;
    return false;
}

static void bx_ln_diag_create_failure(const struct bx_ln_options* options, struct bx_diag_ctx* diag, const char* source_path, const char* destination_path, int err) {
    const char* link_kind = options->symbolic ? "symbolic" : "hard";
    const char* separator = options->symbolic ? "->" : "=>";
    bx_diag(diag, "failed to create %s link '%s' %s '%s': %s", link_kind, destination_path, separator, source_path, strerror(err));
}

static void bx_ln_print_verbose_line(const struct bx_ln_options* options, const char* source_path, const char* destination_path) {
    if (!options->verbose) {
        return;
    }

    if (options->symbolic) {
        printf("'%s' -> '%s'\n", destination_path, source_path);
        return;
    }
    printf("'%s' => '%s'\n", destination_path, source_path);
}

static bool bx_ln_create_link(const struct bx_ln_options* options, struct bx_diag_ctx* diag, const char* source_path, const char* destination_path) {
    int err = 0;

    if (bx_ln_create_link_once(options, source_path, destination_path, &err)) {
        bx_ln_print_verbose_line(options, source_path, destination_path);
        return true;
    }

    if (!(options->force && err == EEXIST)) {
        bx_ln_diag_create_failure(options, diag, source_path, destination_path, err);
        return false;
    }

    if (bx_ln_paths_name_same_directory_entry(source_path, destination_path)) {
        bx_diag(diag, "'%s' and '%s' are the same file", source_path, destination_path);
        return false;
    }

    if (!options->symbolic && bx_ln_hard_link_already_exists(source_path, destination_path, options->follow_symlinks)) {
        bx_ln_print_verbose_line(options, source_path, destination_path);
        return true;
    }

    if (!bx_ln_remove_destination_for_force(destination_path, diag)) {
        return false;
    }

    if (bx_ln_create_link_once(options, source_path, destination_path, &err)) {
        bx_ln_print_verbose_line(options, source_path, destination_path);
        return true;
    }

    bx_ln_diag_create_failure(options, diag, source_path, destination_path, err);
    return false;
}

static char* bx_ln_build_destination_path(const char* source_path, const char* destination_root, bool destination_is_directory) {
    if (!destination_is_directory) {
        return xstrdup(destination_root);
    }

    char* basename = bx_path_basename_dup(source_path);
    char* destination_path = bx_path_join(destination_root, basename);
    free(basename);
    return destination_path;
}

int bx_ln_main(int argc, char** argv) {
    struct bx_ln_options options;
    struct bx_diag_ctx diag = {
        .progname = "ln",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_ln_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_ln_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_ln_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    char** operands = argv + first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing file operand");
        return diag.exit_status;
    }

    if (options.no_target_directory && options.target_directory == NULL) {
        if (operand_count < 2) {
            bx_diag(&diag, "missing destination file operand after '%s'", operands[0]);
            return diag.exit_status;
        }
        if (operand_count > 2) {
            bx_diag(&diag, "extra operand '%s'", operands[2]);
            return diag.exit_status;
        }
    }

    const char* destination_root = NULL;
    bool destination_is_directory = false;
    int source_count = 0;
    char** source_paths = operands;

    if (options.target_directory != NULL) {
        source_count = operand_count;
        destination_root = options.target_directory;
        destination_is_directory = bx_ln_path_is_directory(destination_root, !options.no_dereference);
        if (!destination_is_directory) {
            bx_diag(&diag, "target '%s' is not a directory", destination_root);
            return diag.exit_status;
        }
    }
    else if (operand_count == 1) {
        source_count = 1;
        destination_root = ".";
        destination_is_directory = true;
    }
    else {
        source_count = operand_count - 1;
        destination_root = operands[operand_count - 1];
        destination_is_directory = source_count > 1 || (!options.no_target_directory && bx_ln_path_is_directory(destination_root, !options.no_dereference));

        if (source_count > 1 && !destination_is_directory) {
            bx_diag(&diag, "target '%s' is not a directory", destination_root);
            return diag.exit_status;
        }
    }

    for (int i = 0; i < source_count; i++) {
        const char* source_path = source_paths[i];
        char* destination_path = bx_ln_build_destination_path(source_path, destination_root, destination_is_directory);

        bx_ln_create_link(&options, &diag, source_path, destination_path);
        free(destination_path);
    }

    return diag.exit_status;
}
