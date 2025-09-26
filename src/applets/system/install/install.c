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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/backup_ops.h"
#include "lib/copy_data.h"
#include "lib/mode_parse.h"
#include "lib/path_ops.h"
#include "lib/same_file.h"
#include "bx/diag.h"
#include "bx/libbx.h"

struct bx_install_options {
    const char* progname;
    enum bx_backup_mode backup_mode;
    const char* backup_suffix;
    bool compare;
    bool directory_mode;
    bool make_leading_dirs;
    bool debug;
    bool preserve_timestamps;
    bool strip;
    const char* strip_program;
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
    fprintf(stream, "      --backup[=CONTROL]     make a backup of each existing destination file\n");
    fprintf(stream, "  -b                         like --backup but does not accept an argument\n");
    fprintf(stream, "  -C, --compare               compare source/destination and skip unchanged outputs\n");
    fprintf(stream, "  -c                          (ignored)\n");
    fprintf(stream, "  -d, --directory             treat all operands as directories to create\n");
    fprintf(stream, "  -D                          create all leading components of DEST\n");
    fprintf(stream, "      --debug                 explain how a file is copied (implies -v)\n");
    fprintf(stream, "  -g, --group=GROUP           set group ownership (name or numeric ID)\n");
    fprintf(stream, "  -m, --mode=MODE             set permission mode (octal or symbolic, as in chmod)\n");
    fprintf(stream, "  -o, --owner=OWNER           set owner (name or numeric ID)\n");
    fprintf(stream, "  -p, --preserve-timestamps   apply source atime/mtime to destination\n");
    fprintf(stream, "  -s, --strip                 strip symbol tables after copying files\n");
    fprintf(stream, "      --strip-program=PROGRAM program used to strip binaries\n");
    fprintf(stream, "  -S, --suffix=SUFFIX         override the usual backup suffix\n");
    fprintf(stream, "  -t, --target-directory=DIR  copy all SOURCE arguments into DIR\n");
    fprintf(stream, "  -T, --no-target-directory   treat DEST as a normal file path\n");
    fprintf(stream, "  -v, --verbose               print each created directory and copied file\n");
    fprintf(stream, "      --help                  display this help and exit\n");
    fprintf(stream, "      --version               output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Backup suffix is '~' unless overridden by -S/--suffix or SIMPLE_BACKUP_SUFFIX.\n");
    fprintf(stream, "Backup control for --backup follows VERSION_CONTROL values:\n");
    fprintf(stream, "  none/off, numbered/t, existing/nil, simple/never.\n");
}

static void bx_install_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

#ifdef S_ISVTX
#define BX_INSTALL_STICKY_BIT S_ISVTX
#else
#define BX_INSTALL_STICKY_BIT 01000
#endif
static bool bx_install_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    struct bx_mode_parse_params params = {
        .initial_mode = 0u,
        .result_mask = 07777u,
        .max_numeric_mode = 07777u,
        .umask_value = 0u,
        .sticky_bit = BX_INSTALL_STICKY_BIT,
        .x_policy = BX_MODE_X_DISABLED,
        .is_directory = false,
        .apply_umask_when_who_omitted = false,
        .allow_setuid = true,
        .allow_setgid = true,
        .allow_sticky = true,
    };

    if (bx_mode_parse(text, &params, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
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

enum {
    BX_INSTALL_OPT_BACKUP = 256,
    BX_INSTALL_OPT_DEBUG,
    BX_INSTALL_OPT_STRIP_PROGRAM,
};

static bool bx_install_parse_options(int argc, char** argv, struct bx_install_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"backup", optional_argument, NULL, BX_INSTALL_OPT_BACKUP},
        {"compare", no_argument, NULL, 'C'},
        {"directory", no_argument, NULL, 'd'},
        {"debug", no_argument, NULL, BX_INSTALL_OPT_DEBUG},
        {"group", required_argument, NULL, 'g'},
        {"mode", required_argument, NULL, 'm'},
        {"owner", required_argument, NULL, 'o'},
        {"preserve-timestamps", no_argument, NULL, 'p'},
        {"strip", no_argument, NULL, 's'},
        {"strip-program", required_argument, NULL, BX_INSTALL_OPT_STRIP_PROGRAM},
        {"suffix", required_argument, NULL, 'S'},
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
        int c = getopt_long(argc, argv, ":bCcDdg:m:o:psS:t:Tv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'b':
                bx_args_enable_backup_mode(&options->backup_mode);
                break;
            case BX_INSTALL_OPT_BACKUP:
                if (optarg != NULL) {
                    if (!bx_args_parse_backup_mode(optarg, &options->backup_mode)) {
                        bx_diag(diag, "invalid --backup control value '%s'", optarg);
                        return false;
                    }
                }
                else {
                    bx_args_enable_backup_mode(&options->backup_mode);
                }
                break;
            case 'C':
                options->compare = true;
                break;
            case 'c':
                /* GNU install accepts -c as a no-op compatibility option. */
                break;
            case 'D':
                options->make_leading_dirs = true;
                break;
            case 'd':
                options->directory_mode = true;
                break;
            case BX_INSTALL_OPT_DEBUG:
                options->debug = true;
                options->verbose = true;
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
            case 's':
                options->strip = true;
                break;
            case BX_INSTALL_OPT_STRIP_PROGRAM:
                options->strip_program = optarg;
                break;
            case 'S':
                options->backup_suffix = optarg;
                bx_args_enable_backup_mode(&options->backup_mode);
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

    if (options->compare && options->strip) {
        bx_diag(diag, "options --compare (-C) and --strip are mutually exclusive");
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

static bool
bx_install_mkdir_p(const char* path, mode_t final_mode, bool set_final_mode, bool apply_final_owner_group, const struct bx_install_options* options, bool verbose, struct bx_diag_ctx* diag) {
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
            if (final_component && apply_final_owner_group && !bx_install_apply_owner_group_path(normalized, options->owner_set, options->owner, options->group_set, options->group, diag)) {
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
                if (apply_final_owner_group && !bx_install_apply_owner_group_path(normalized, options->owner_set, options->owner, options->group_set, options->group, diag)) {
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

static bool bx_install_times_equal(const struct timespec* left, const struct timespec* right) {
    return left->tv_sec == right->tv_sec && left->tv_nsec == right->tv_nsec;
}

static bool bx_install_destination_matches_compare_requirements(const struct stat* src_st, const struct stat* dest_st, const struct bx_install_options* options) {
    mode_t expected_mode = options->mode_set ? options->mode : 0755u;
    if ((dest_st->st_mode & 07777u) != expected_mode) {
        return false;
    }

    if (options->owner_set && dest_st->st_uid != options->owner) {
        return false;
    }

    if (options->group_set && dest_st->st_gid != options->group) {
        return false;
    }

    if (options->preserve_timestamps && (!bx_install_times_equal(&src_st->st_atim, &dest_st->st_atim) || !bx_install_times_equal(&src_st->st_mtim, &dest_st->st_mtim))) {
        return false;
    }

    return true;
}

static bool bx_install_compare_file_contents_fd(int src_fd,
                                                const char* src_path,
                                                int dest_fd,
                                                const char* dest_path,
                                                struct bx_diag_ctx* diag,
                                                bool* equal_out) {
    bool equal = false;
    unsigned char src_buf[65536];
    unsigned char dest_buf[65536];
    for (;;) {
        ssize_t src_read = -1;
        do {
            src_read = read(src_fd, src_buf, sizeof(src_buf));
        } while (src_read < 0 && errno == EINTR);
        if (src_read < 0) {
            bx_perror_path(diag, src_path);
            goto fail;
        }

        ssize_t dest_read = -1;
        do {
            dest_read = read(dest_fd, dest_buf, sizeof(dest_buf));
        } while (dest_read < 0 && errno == EINTR);
        if (dest_read < 0) {
            bx_perror_path(diag, dest_path);
            goto fail;
        }

        if (src_read != dest_read) {
            equal = false;
            break;
        }

        if (src_read == 0) {
            equal = true;
            break;
        }

        if (memcmp(src_buf, dest_buf, (size_t)src_read) != 0) {
            equal = false;
            break;
        }
    }

    *equal_out = equal;
    return true;

fail:
    return false;
}

static bool bx_install_apply_file_metadata_fd(int dest_fd,
                                              const char* dest_path,
                                              const struct stat* src_st,
                                              const struct bx_install_options* options,
                                              struct bx_diag_ctx* diag) {
    if (!bx_install_apply_owner_group_fd(dest_fd, dest_path, options, diag)) {
        return false;
    }

    mode_t file_mode = options->mode_set ? options->mode : 0755u;
    if (fchmod(dest_fd, file_mode) != 0) {
        bx_perror_path(diag, dest_path);
        return false;
    }

    if (options->preserve_timestamps && src_st != NULL) {
        struct timespec ts[2] = {src_st->st_atim, src_st->st_mtim};
        if (futimens(dest_fd, ts) != 0) {
            bx_perror_path(diag, dest_path);
            return false;
        }
    }

    return true;
}

static bool bx_install_update_existing_destination(const char* src_path,
                                                   const char* dest_path,
                                                   const struct stat* src_st,
                                                   const struct bx_install_options* options,
                                                   struct bx_diag_ctx* diag) {
    int dest_fd = open(dest_path, O_WRONLY);
    if (dest_fd < 0) {
        bx_perror_path(diag, dest_path);
        return false;
    }

    bool ok = bx_install_apply_file_metadata_fd(dest_fd, dest_path, src_st, options, diag);
    if (close(dest_fd) != 0) {
        bx_perror_path(diag, dest_path);
        ok = false;
    }

    if (ok && options->verbose && !bx_install_emit_copy(src_path, dest_path, diag)) {
        return false;
    }

    return ok;
}

static enum bx_sparse_mode bx_install_copy_sparse_mode_for_source(const struct stat* src_st) {
    if (src_st == NULL || !S_ISREG(src_st->st_mode) || src_st->st_size <= 0) {
        return BX_SPARSE_AUTO;
    }

    uintmax_t allocated_bytes = (uintmax_t)src_st->st_blocks * 512u;
    if (allocated_bytes >= (uintmax_t)src_st->st_size) {
        return BX_SPARSE_NEVER;
    }

    return BX_SPARSE_AUTO;
}

static bool bx_install_run_strip(const char* strip_program, const char* path, struct bx_diag_ctx* diag) {
    const char* program = (strip_program != NULL && strip_program[0] != '\0') ? strip_program : "strip";

    pid_t pid = fork();
    if (pid < 0) {
        bx_diag(diag, "failed to start strip program '%s': %s", program, strerror(errno));
        return false;
    }

    if (pid == 0) {
        execlp(program, program, path, (char*)NULL);
        _exit(errno == ENOENT ? 127 : 126);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        bx_diag(diag, "failed to wait for strip: %s", strerror(errno));
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return true;
    }

    bx_diag(diag, "strip program '%s' terminated abnormally", program);
    return false;
}

static bool bx_install_copy_regular_file(const char* src_path,
                                         const char* dest_path,
                                         const struct bx_install_options* options,
                                         const struct bx_backup_params* backup_params,
                                         struct bx_diag_ctx* diag) {
    struct stat src_st;
    struct stat dest_st;
    bool dest_exists = false;
    bool dest_created = false;
    char* backup_path = NULL;
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

    src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        bx_perror_path(diag, src_path);
        return false;
    }

    dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (dest_fd >= 0) {
        dest_created = true;
    }
    else if (errno != EEXIST && errno != EISDIR) {
        bx_perror_path(diag, dest_path);
        goto fail_keep;
    }
    else if (lstat(dest_path, &dest_st) == 0) {
        dest_exists = true;
        if (S_ISDIR(dest_st.st_mode)) {
            errno = EISDIR;
            bx_perror_path(diag, dest_path);
            goto fail_keep;
        }
        if (!S_ISLNK(dest_st.st_mode) && bx_same_file(&src_st, &dest_st)) {
            bx_diag(diag, "'%s' and '%s' are the same file", src_path, dest_path);
            goto fail_keep;
        }
    }
    else if (errno != ENOENT) {
        bx_perror_path(diag, dest_path);
        goto fail_keep;
    }

    if (options->compare && dest_exists && S_ISREG(dest_st.st_mode) && src_st.st_size == dest_st.st_size) {
        bool same_contents = false;
        int compare_dest_fd = -1;

        compare_dest_fd = open(dest_path, O_RDONLY);
        if (compare_dest_fd < 0) {
            bx_perror_path(diag, dest_path);
            goto fail_keep;
        }

        if (!bx_install_compare_file_contents_fd(src_fd, src_path, compare_dest_fd, dest_path, diag, &same_contents)) {
            (void)close(compare_dest_fd);
            goto fail_keep;
        }

        if (close(compare_dest_fd) != 0) {
            bx_perror_path(diag, dest_path);
            goto fail_keep;
        }

        if (same_contents) {
            if (close(src_fd) != 0) {
                bx_perror_path(diag, src_path);
                src_fd = -1;
                goto fail_keep;
            }
            src_fd = -1;

            if (bx_install_destination_matches_compare_requirements(&src_st, &dest_st, options)) {
                return true;
            }

            return bx_install_update_existing_destination(src_path, dest_path, &src_st, options, diag);
        }

        if (lseek(src_fd, 0, SEEK_SET) < 0) {
            bx_perror_path(diag, src_path);
            goto fail_keep;
        }
    }

    if (dest_exists) {
        if (bx_args_backup_mode_enabled(backup_params->mode)) {
            enum bx_backup_create_result backup_result = bx_backup_create(dest_path, backup_params, diag, &backup_path);
            if (backup_result == BX_BACKUP_CREATE_FAILED) {
                goto fail_keep;
            }
        }
        else if (unlink(dest_path) != 0) {
            bx_perror_path(diag, dest_path);
            goto fail_keep;
        }
    }

    if (!dest_created) {
        dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
        if (dest_fd < 0) {
            bx_perror_path(diag, dest_path);
            goto fail_keep;
        }
        dest_created = true;
    }

    struct bx_copy_data_options copy_data_options = {
        .sparse_mode = bx_install_copy_sparse_mode_for_source(&src_st),
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

    if (options->preserve_timestamps && !options->strip) {
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

    if (options->strip && !bx_install_run_strip(options->strip_program, dest_path, diag)) {
        goto fail_remove;
    }

    if (options->preserve_timestamps && options->strip) {
        struct timespec ts[2] = {src_st.st_atim, src_st.st_mtim};
        if (utimensat(AT_FDCWD, dest_path, ts, 0) != 0) {
            bx_perror_path(diag, dest_path);
            goto fail_remove;
        }
    }

    if (options->verbose && !bx_install_emit_copy(src_path, dest_path, diag)) {
        free(backup_path);
        return false;
    }

    free(backup_path);
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
    free(backup_path);
    return false;

fail_keep:
    if (dest_fd >= 0) {
        (void)close(dest_fd);
    }
    if (src_fd >= 0) {
        (void)close(src_fd);
    }
    free(backup_path);
    return false;
}

int bx_install_main(int argc, char** argv) {
    struct bx_install_options options;
    struct bx_backup_params backup_params;
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
    diag.debug = options.debug;
    bx_backup_get_params(options.backup_mode, options.backup_suffix, &backup_params);

    if (options.directory_mode) {
        if (operand_count <= 0) {
            bx_diag(&diag, "missing operand");
            return diag.exit_status;
        }

        for (int i = first_operand; i < argc; i++) {
            (void)bx_install_mkdir_p(argv[i], install_mode, true, true, &options, options.verbose, &diag);
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

    if (destination_is_directory) {
        if (options.make_leading_dirs && options.target_directory != NULL) {
            if (!bx_install_mkdir_p(destination_root, 0755u, false, false, &options, options.verbose, &diag)) {
                return diag.exit_status;
            }
        }

        if (!bx_install_validate_directory_target(destination_root, &diag)) {
            return diag.exit_status;
        }
    }

    for (int i = 0; i < source_count; i++) {
        const char* source_path = argv[first_operand + i];
        char* dest_path = bx_path_build_dest(source_path, destination_root, destination_is_directory, false);

        if (options.make_leading_dirs) {
            char* parent_dir = bx_install_parent_dir_dup(dest_path);
            if (parent_dir != NULL) {
                if (!bx_install_mkdir_p(parent_dir, 0755u, false, false, &options, options.verbose, &diag)) {
                    free(parent_dir);
                    free(dest_path);
                    continue;
                }
                free(parent_dir);
            }
        }

        (void)bx_install_copy_regular_file(source_path, dest_path, &options, &backup_params, &diag);
        free(dest_path);
    }

    if (options.verbose && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
