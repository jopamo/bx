#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "common/copy_data.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "diag.h"
#include "libbx.h"

struct bx_install_options {
    const char* progname;
    bool directory_mode;
    bool make_leading_dirs;
    bool preserve_timestamps;
    bool mode_set;
    mode_t mode;
    bool owner_set;
    uid_t owner;
    bool group_set;
    gid_t group;
    const char* target_directory;
    bool no_target_directory;
    bool verbose;
    bool show_help;
    bool show_version;
};

static const char* bx_install_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "install";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_install_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [-T] SOURCE DEST\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... SOURCE... DIRECTORY\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -d DIRECTORY...\n", progname);
    fprintf(stream, "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n");
    fprintf(stream, "Create all components of DIRECTORY(ies) with -d.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported options:\n");
    fprintf(stream, "  -d, --directory             treat all operands as directories to create\n");
    fprintf(stream, "  -D                          create all leading components of DEST\n");
    fprintf(stream, "  -g, --group=GROUP           set group ownership (name or numeric ID)\n");
    fprintf(stream, "  -m, --mode=MODE             set permission mode (octal)\n");
    fprintf(stream, "  -o, --owner=OWNER           set owner (name or numeric ID)\n");
    fprintf(stream, "  -p, --preserve-timestamps   apply source atime/mtime to destination\n");
    fprintf(stream, "  -t, --target-directory=DIR  copy all SOURCE arguments into DIR\n");
    fprintf(stream, "  -T, --no-target-directory   treat DEST as a normal file path\n");
    fprintf(stream, "  -v, --verbose               print each created directory and copied file\n");
    fprintf(stream, "      --help                  display this help and exit\n");
    fprintf(stream, "      --version               output version information and exit\n");
}

static void bx_install_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_install_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 8);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0' || value > 07777ul) {
        bx_diag(diag, "invalid mode '%s'", text);
        return false;
    }

    *mode_out = (mode_t)value;
    return true;
}

static bool bx_install_parse_id_numeric(const char* text, uintmax_t max_value, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }
    if ((uintmax_t)value > max_value) {
        return false;
    }

    *value_out = (uintmax_t)value;
    return true;
}

static bool bx_install_parse_owner(const char* text, uid_t* owner_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_install_parse_id_numeric(text, (uintmax_t)((uid_t)-1), &numeric_id)) {
        *owner_out = (uid_t)numeric_id;
        return true;
    }

    struct passwd* passwd_entry = getpwnam(text);
    if (passwd_entry != NULL) {
        *owner_out = passwd_entry->pw_uid;
        return true;
    }

    bx_diag(diag, "invalid user '%s'", (text != NULL) ? text : "");
    return false;
}

static bool bx_install_parse_group(const char* text, gid_t* group_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_install_parse_id_numeric(text, (uintmax_t)((gid_t)-1), &numeric_id)) {
        *group_out = (gid_t)numeric_id;
        return true;
    }

    struct group* group_entry = getgrnam(text);
    if (group_entry != NULL) {
        *group_out = group_entry->gr_gid;
        return true;
    }

    bx_diag(diag, "invalid group '%s'", (text != NULL) ? text : "");
    return false;
}

static bool bx_install_apply_owner_group_fd(int fd, const char* path, const struct bx_install_options* options, struct bx_diag_ctx* diag) {
    if (!options->owner_set && !options->group_set) {
        return true;
    }

    uid_t owner = options->owner_set ? options->owner : (uid_t)-1;
    gid_t group = options->group_set ? options->group : (gid_t)-1;
    if (fchown(fd, owner, group) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static bool bx_install_apply_owner_group_path(const char* path, bool owner_set, uid_t owner, bool group_set, gid_t group, struct bx_diag_ctx* diag) {
    if (!owner_set && !group_set) {
        return true;
    }

    uid_t resolved_owner = owner_set ? owner : (uid_t)-1;
    gid_t resolved_group = group_set ? group : (gid_t)-1;
    if (chown(path, resolved_owner, resolved_group) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static bool bx_install_parse_options(int argc, char** argv, struct bx_install_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"directory", no_argument, NULL, 'd'},
        {"group", required_argument, NULL, 'g'},
        {"mode", required_argument, NULL, 'm'},
        {"owner", required_argument, NULL, 'o'},
        {"preserve-timestamps", no_argument, NULL, 'p'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_install_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:Ddg:m:o:pt:Tv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'D':
                options->make_leading_dirs = true;
                break;
            case 'd':
                options->directory_mode = true;
                break;
            case 'g':
                if (!bx_install_parse_group(optarg, &options->group, diag)) {
                    return false;
                }
                options->group_set = true;
                break;
            case 'm':
                if (!bx_install_parse_mode(optarg, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case 'o':
                if (!bx_install_parse_owner(optarg, &options->owner, diag)) {
                    return false;
                }
                options->owner_set = true;
                break;
            case 'p':
                options->preserve_timestamps = true;
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
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
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

    if (options->target_directory != NULL && options->directory_mode) {
        bx_diag(diag, "target directory not allowed when installing a directory");
        return false;
    }

    if (options->target_directory != NULL && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory and --no-target-directory");
        return false;
    }

    *first_operand = optind;
    return true;
}

static bool bx_install_emit_created_dir(const char* path, struct bx_diag_ctx* diag) {
    if (fprintf(stdout, "install: creating directory '%s'\n", path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_install_emit_copy(const char* src_path, const char* dest_path, struct bx_diag_ctx* diag) {
    if (fprintf(stdout, "'%s' -> '%s'\n", src_path, dest_path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_install_mkdir_p(const char* path, mode_t final_mode, bool set_final_mode, bool verbose, struct bx_diag_ctx* diag) {
    char* normalized = bx_path_strip_trailing_slashes_dup(path);
    if (normalized[0] == '\0') {
        errno = ENOENT;
        bx_perror_path(diag, path);
        free(normalized);
        return false;
    }

    if (strcmp(normalized, "/") == 0) {
        free(normalized);
        return true;
    }

    size_t len = strlen(normalized);
    size_t start = (normalized[0] == '/') ? 1u : 0u;
    bool processed = false;

    for (size_t i = start; i <= len; i++) {
        if (normalized[i] != '/' && normalized[i] != '\0') {
            continue;
        }
        if (i > start && normalized[i - 1] == '/') {
            continue;
        }

        char saved = normalized[i];
        normalized[i] = '\0';
        bool final_component = (saved == '\0');
        mode_t create_mode = (final_component && set_final_mode) ? final_mode : 0755u;

        if (mkdir(normalized, create_mode) == 0) {
            if (final_component && !bx_install_apply_owner_group_path(normalized, options->owner_set, options->owner, options->group_set, options->group, diag)) {
                free(normalized);
                return false;
            }
            if (chmod(normalized, create_mode) != 0) {
                bx_perror_path(diag, normalized);
                free(normalized);
                return false;
            }
            if (verbose && !bx_install_emit_created_dir(normalized, diag)) {
                free(normalized);
                return false;
            }
        }
        else if (errno == EEXIST) {
            struct stat st;
            if (stat(normalized, &st) != 0) {
                bx_perror_path(diag, normalized);
                free(normalized);
                return false;
            }
            if (!S_ISDIR(st.st_mode)) {
                errno = ENOTDIR;
                bx_perror_path(diag, normalized);
                free(normalized);
                return false;
            }
            if (final_component && set_final_mode) {
                if (!bx_install_apply_owner_group_path(normalized, options->owner_set, options->owner, options->group_set, options->group, diag)) {
                    free(normalized);
                    return false;
                }
                if (chmod(normalized, final_mode) != 0) {
                    bx_perror_path(diag, normalized);
                    free(normalized);
                    return false;
                }
            }
        }
        else {
            bx_perror_path(diag, normalized);
            free(normalized);
            return false;
        }

        normalized[i] = saved;
        processed = true;
    }

    if (!processed) {
        errno = ENOENT;
        bx_perror_path(diag, path);
        free(normalized);
        return false;
    }

    free(normalized);
    return true;
}

static char* bx_install_parent_dir_dup(const char* path) {
    char* stripped = bx_path_strip_trailing_slashes_dup(path);
    char* slash = strrchr(stripped, '/');
    if (slash == NULL) {
        free(stripped);
        return NULL;
    }

    if (slash == stripped) {
        slash[1] = '\0';
        return stripped;
    }

    *slash = '\0';
    char* parent = bx_path_strip_trailing_slashes_dup(stripped);
    free(stripped);
    return parent;
}

static bool bx_install_validate_directory_target(const char* path, struct bx_diag_ctx* diag) {
    struct stat st;
    if (stat(path, &st) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

static bool bx_install_copy_regular_file(const char* src_path, const char* dest_path, const struct bx_install_options* options, struct bx_diag_ctx* diag) {
    struct stat src_st;
    struct stat dest_st;
    bool dest_exists = false;
    bool dest_created = false;
    int src_fd = -1;
    int dest_fd = -1;

    if (stat(src_path, &src_st) != 0) {
        bx_perror_path(diag, src_path);
        return false;
    }

    if (!S_ISREG(src_st.st_mode)) {
        bx_diag(diag, "omitting non-regular file '%s'", src_path);
        return false;
    }

    if (lstat(dest_path, &dest_st) == 0) {
        dest_exists = true;
        if (S_ISDIR(dest_st.st_mode)) {
            errno = EISDIR;
            bx_perror_path(diag, dest_path);
            return false;
        }
        if (!S_ISLNK(dest_st.st_mode) && bx_same_file(&src_st, &dest_st)) {
            bx_diag(diag, "'%s' and '%s' are the same file", src_path, dest_path);
            return false;
        }
    }
    else if (errno != ENOENT) {
        bx_perror_path(diag, dest_path);
        return false;
    }

    src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        bx_perror_path(diag, src_path);
        return false;
    }

    if (dest_exists && unlink(dest_path) != 0) {
        bx_perror_path(diag, dest_path);
        goto fail_keep;
    }

    dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (dest_fd < 0) {
        bx_perror_path(diag, dest_path);
        goto fail_keep;
    }
    dest_created = true;

    struct bx_copy_data_options copy_data_options = {
        .sparse_mode = BX_SPARSE_AUTO,
        .reflink_mode = BX_REFLINK_NEVER,
    };
    int copy_result = bx_copy_data(src_fd, dest_fd, &copy_data_options);
    if (copy_result == BX_COPY_DATA_READ_ERROR) {
        bx_perror_path(diag, src_path);
        goto fail_remove;
    }
    if (copy_result == BX_COPY_DATA_WRITE_ERROR) {
        bx_perror_path(diag, dest_path);
        goto fail_remove;
    }
    if (copy_result != BX_COPY_DATA_SUCCESS) {
        bx_diag(diag, "failed to copy '%s' to '%s'", src_path, dest_path);
        goto fail_remove;
    }

    if (!bx_install_apply_owner_group_fd(dest_fd, dest_path, options, diag)) {
        goto fail_keep;
    }

    mode_t file_mode = options->mode_set ? options->mode : 0755u;
    if (fchmod(dest_fd, file_mode) != 0) {
        bx_perror_path(diag, dest_path);
        goto fail_remove;
    }

    if (options->preserve_timestamps) {
        struct timespec ts[2] = {src_st.st_atim, src_st.st_mtim};
        if (futimens(dest_fd, ts) != 0) {
            bx_perror_path(diag, dest_path);
            goto fail_remove;
        }
    }

    if (close(dest_fd) != 0) {
        bx_perror_path(diag, dest_path);
        dest_fd = -1;
        goto fail_keep;
    }
    dest_fd = -1;

    if (close(src_fd) != 0) {
        bx_perror_path(diag, src_path);
        src_fd = -1;
        goto fail_keep;
    }
    src_fd = -1;

    if (options->verbose && !bx_install_emit_copy(src_path, dest_path, diag)) {
        return false;
    }

    return true;

fail_remove:
    if (dest_fd >= 0) {
        (void)close(dest_fd);
    }
    if (src_fd >= 0) {
        (void)close(src_fd);
    }
    if (dest_created) {
        (void)unlink(dest_path);
    }
    return false;

fail_keep:
    if (dest_fd >= 0) {
        (void)close(dest_fd);
    }
    if (src_fd >= 0) {
        (void)close(src_fd);
    }
    return false;
}

int bx_install_main(int argc, char** argv) {
    struct bx_install_options options;
    struct bx_diag_ctx diag = {
        .progname = "install",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_install_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_install_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_install_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    mode_t install_mode = options.mode_set ? options.mode : 0755u;
    diag.verbose = options.verbose;

    if (options.directory_mode) {
        if (operand_count <= 0) {
            bx_diag(&diag, "missing operand");
            return diag.exit_status;
        }

        for (int i = first_operand; i < argc; i++) {
            (void)bx_install_mkdir_p(argv[i], install_mode, true, options.verbose, &diag);
        }

        if (options.verbose && fflush(stdout) == EOF) {
            bx_diag(&diag, "write error: %s", strerror(errno));
        }
        return diag.exit_status;
    }

    if (operand_count <= 0) {
        bx_diag(&diag, "missing file operand");
        return diag.exit_status;
    }

    const char* destination_root = NULL;
    int source_count = 0;
    bool destination_is_directory = false;

    if (options.target_directory != NULL) {
        destination_root = options.target_directory;
        source_count = operand_count;
        destination_is_directory = true;
    }
    else {
        if (operand_count < 2) {
            bx_diag(&diag, "missing destination file operand after '%s'", argv[first_operand]);
            return diag.exit_status;
        }

        destination_root = argv[argc - 1];
        source_count = operand_count - 1;
        if (options.no_target_directory) {
            if (source_count != 1) {
                bx_diag(&diag, "extra operand '%s'", argv[first_operand + 1]);
                return diag.exit_status;
            }
        }
        else if (source_count > 1) {
            destination_is_directory = true;
        }
        else {
            struct stat destination_stat;
            if (stat(destination_root, &destination_stat) == 0 && S_ISDIR(destination_stat.st_mode)) {
                destination_is_directory = true;
            }
        }
    }

    if (options.make_leading_dirs && source_count != 1) {
        bx_diag(&diag, "cannot use -D with multiple sources");
        return diag.exit_status;
    }

    if (destination_is_directory && !bx_install_validate_directory_target(destination_root, &diag)) {
        return diag.exit_status;
    }

    for (int i = 0; i < source_count; i++) {
        const char* source_path = argv[first_operand + i];
        char* dest_path = bx_path_build_dest(source_path, destination_root, destination_is_directory, false);

        if (options.make_leading_dirs) {
            char* parent_dir = bx_install_parent_dir_dup(dest_path);
            if (parent_dir != NULL) {
                if (!bx_install_mkdir_p(parent_dir, 0755u, false, options.verbose, &diag)) {
                    free(parent_dir);
                    free(dest_path);
                    continue;
                }
                free(parent_dir);
            }
        }

        (void)bx_install_copy_regular_file(source_path, dest_path, &options, &diag);
        free(dest_path);
    }

    if (options.verbose && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
