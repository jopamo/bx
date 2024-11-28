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
#include "common/args_common.h"
#include "common/backup_ops.h"
#include "common/path_ops.h"
#include "common/prompt_ops.h"
#include "common/same_file.h"
#include "diag.h"
#include "libbx.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

struct bx_ln_options {
    const char* progname;
    bool symbolic;
    bool relative;
    bool allow_directory_hard_links;
    bool force;
    bool interactive;
    bool no_dereference;
    bool no_target_directory;
    bool verbose;
    bool follow_symlinks;
    const char* target_directory;
    enum bx_backup_mode backup_mode;
    const char* suffix;
    bool show_help;
    bool show_version;
};

enum {
    BX_LN_OPT_BACKUP = 256,
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
    fprintf(stream, "      --backup[=CONTROL]     make a backup of each existing destination file\n");
    fprintf(stream, "  -b                         like --backup but does not accept an argument\n");
    fprintf(stream, "  -d, -F, --directory        allow hard links to directories\n");
    fprintf(stream, "  -i, --interactive          prompt whether to remove destinations\n");
    fprintf(stream, "  -L, --logical              dereference TARGETs that are symbolic links\n");
    fprintf(stream, "  -n, --no-dereference       treat LINK_NAME as a normal file if it is a symbolic link\n");
    fprintf(stream, "  -P, --physical             make hard links directly to symbolic links (default)\n");
    fprintf(stream, "  -r, --relative             with -s, create links relative to link location\n");
    fprintf(stream, "  -s, --symbolic             make symbolic links instead of hard links\n");
    fprintf(stream, "  -S, --suffix=SUFFIX        override the usual backup suffix\n");
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
        {"backup", optional_argument, NULL, BX_LN_OPT_BACKUP},
        {"directory", no_argument, NULL, 'd'},
        {"force", no_argument, NULL, 'f'},
        {"interactive", no_argument, NULL, 'i'},
        {"logical", no_argument, NULL, 'L'},
        {"no-dereference", no_argument, NULL, 'n'},
        {"physical", no_argument, NULL, 'P'},
        {"relative", no_argument, NULL, 'r'},
        {"symbolic", no_argument, NULL, 's'},
        {"suffix", required_argument, NULL, 'S'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };
    char short_opts[] = "+:bdFfiLnPrsS:t:Tv";

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
            case 'b':
                bx_args_enable_backup_mode(&options->backup_mode);
                break;
            case BX_LN_OPT_BACKUP:
                if (optarg == NULL) {
                    bx_args_enable_backup_mode(&options->backup_mode);
                }
                else if (!bx_args_parse_backup_mode(optarg, &options->backup_mode)) {
                    bx_diag(diag, "invalid --backup control value '%s'", optarg);
                    return false;
                }
                break;
            case 'd':
            case 'F':
                options->allow_directory_hard_links = true;
                break;
            case 'f':
                options->force = true;
                options->interactive = false;
                break;
            case 'i':
                options->interactive = true;
                options->force = false;
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
            case 'r':
                options->relative = true;
                break;
            case 's':
                options->symbolic = true;
                break;
            case 'S':
                options->suffix = optarg;
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

    if (options->relative && !options->symbolic) {
        bx_diag(diag, "cannot do --relative without --symbolic");
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

struct bx_ln_components {
    char** parts;
    size_t count;
};

static void bx_ln_components_push(struct bx_ln_components* components, const char* part) {
    components->parts = xrealloc(components->parts, sizeof(*components->parts) * (components->count + 1u));
    components->parts[components->count++] = xstrdup(part);
}

static void bx_ln_components_pop(struct bx_ln_components* components) {
    if (components->count == 0) {
        return;
    }
    free(components->parts[components->count - 1u]);
    components->count--;
}

static void bx_ln_components_free(struct bx_ln_components* components) {
    for (size_t i = 0; i < components->count; i++) {
        free(components->parts[i]);
    }
    free(components->parts);
    components->parts = NULL;
    components->count = 0;
}

static void bx_ln_components_append_normalized(struct bx_ln_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            bx_ln_components_pop(components);
            continue;
        }
        bx_ln_components_push(components, token);
    }

    free(copy);
}

static char* bx_ln_components_to_absolute_path(const struct bx_ln_components* components) {
    if (components->count == 0) {
        return xstrdup("/");
    }

    size_t len = 2u;
    for (size_t i = 0; i < components->count; i++) {
        len += strlen(components->parts[i]);
        if (i + 1u < components->count) {
            len++;
        }
    }

    char* path = xmalloc(len);
    size_t pos = 0;
    path[pos++] = '/';
    for (size_t i = 0; i < components->count; i++) {
        size_t part_len = strlen(components->parts[i]);
        memcpy(path + pos, components->parts[i], part_len);
        pos += part_len;
        if (i + 1u < components->count) {
            path[pos++] = '/';
        }
    }
    path[pos] = '\0';
    return path;
}

static char* bx_ln_getcwd_dup(void) {
    size_t size = 128u;
    char* cwd = xmalloc(size);

    while (getcwd(cwd, size) == NULL) {
        if (errno != ERANGE) {
            free(cwd);
            return NULL;
        }
        size *= 2u;
        cwd = xrealloc(cwd, size);
    }

    return cwd;
}

static char* bx_ln_normalize_absolute_lexical(const char* path) {
    struct bx_ln_components components = {0};
    char* cwd = NULL;
    char* normalized = NULL;

    if (path[0] != '/') {
        cwd = bx_ln_getcwd_dup();
        if (cwd == NULL) {
            return NULL;
        }
        bx_ln_components_append_normalized(&components, cwd);
    }

    bx_ln_components_append_normalized(&components, path);
    normalized = bx_ln_components_to_absolute_path(&components);

    free(cwd);
    bx_ln_components_free(&components);
    return normalized;
}

static bool bx_ln_parent_dir_dup(const char* path, char** parent_out) {
    char* copy = xstrdup(path);
    char* slash = strrchr(copy, '/');

    if (slash == NULL) {
        free(copy);
        *parent_out = xstrdup(".");
        return true;
    }
    if (slash == copy) {
        slash[1] = '\0';
        *parent_out = copy;
        return true;
    }

    *slash = '\0';
    *parent_out = copy;
    return true;
}

static char* bx_ln_canonicalize_for_relative(const char* path) {
    char* canonical = realpath(path, NULL);
    if (canonical != NULL) {
        return canonical;
    }

    if (errno == ENOENT || errno == ENOTDIR) {
        char* parent = NULL;
        char* parent_real = NULL;
        char* base = NULL;
        char* joined = NULL;
        char* normalized = NULL;

        if (!bx_ln_parent_dir_dup(path, &parent)) {
            return NULL;
        }

        parent_real = realpath(parent, NULL);
        if (parent_real != NULL) {
            base = bx_path_basename_dup(path);
            joined = bx_path_join(parent_real, base);
            normalized = bx_ln_normalize_absolute_lexical(joined);
            free(joined);
            free(base);
            free(parent_real);
            free(parent);
            return normalized;
        }

        free(parent);
        return bx_ln_normalize_absolute_lexical(path);
    }

    return NULL;
}

static char* bx_ln_relative_path_between(const char* from_abs, const char* to_abs) {
    struct bx_ln_components from_components = {0};
    struct bx_ln_components to_components = {0};
    char* relative = NULL;
    size_t common = 0;

    if (from_abs[0] != '/' || to_abs[0] != '/') {
        return NULL;
    }

    bx_ln_components_append_normalized(&from_components, from_abs);
    bx_ln_components_append_normalized(&to_components, to_abs);

    while (common < from_components.count && common < to_components.count && strcmp(from_components.parts[common], to_components.parts[common]) == 0) {
        common++;
    }

    size_t up_count = from_components.count - common;
    size_t down_count = to_components.count - common;
    size_t segment_count = up_count + down_count;
    if (segment_count == 0) {
        relative = xstrdup(".");
        goto out;
    }

    size_t len = 1u;
    if (segment_count > 1u) {
        len += segment_count - 1u;
    }
    len += up_count * 2u;
    for (size_t i = common; i < to_components.count; i++) {
        len += strlen(to_components.parts[i]);
    }

    relative = xmalloc(len);
    size_t pos = 0;
    for (size_t i = 0; i < up_count; i++) {
        if (pos > 0) {
            relative[pos++] = '/';
        }
        relative[pos++] = '.';
        relative[pos++] = '.';
    }
    for (size_t i = common; i < to_components.count; i++) {
        if (pos > 0) {
            relative[pos++] = '/';
        }
        size_t part_len = strlen(to_components.parts[i]);
        memcpy(relative + pos, to_components.parts[i], part_len);
        pos += part_len;
    }
    relative[pos] = '\0';

out:
    bx_ln_components_free(&from_components);
    bx_ln_components_free(&to_components);
    return relative;
}

static char* bx_ln_make_relative_source_path(const char* source_path, const char* destination_path, struct bx_diag_ctx* diag) {
    char* destination_parent = NULL;
    char* destination_parent_abs = NULL;
    char* source_abs = NULL;
    char* relative = NULL;
    int saved_errno;

    source_abs = bx_ln_canonicalize_for_relative(source_path);
    if (source_abs == NULL) {
        saved_errno = errno;
        bx_diag(diag, "cannot resolve source '%s' for relative link: %s", source_path, strerror(saved_errno));
        return NULL;
    }

    bx_ln_parent_dir_dup(destination_path, &destination_parent);
    destination_parent_abs = bx_ln_canonicalize_for_relative(destination_parent);
    if (destination_parent_abs == NULL) {
        saved_errno = errno;
        bx_diag(diag, "cannot resolve destination directory '%s' for relative link: %s", destination_parent, strerror(saved_errno));
        free(destination_parent);
        free(source_abs);
        return NULL;
    }

    relative = bx_ln_relative_path_between(destination_parent_abs, source_abs);
    if (relative == NULL) {
        bx_diag(diag, "cannot build relative path from '%s' to '%s'", destination_parent_abs, source_abs);
    }

    free(destination_parent_abs);
    free(destination_parent);
    free(source_abs);
    return relative;
}

static bool bx_ln_parent_dir_stat(const char* path, struct stat* parent_stat_out) {
    char* stripped = bx_path_strip_trailing_slashes_dup(path);
    char* parent_path = NULL;
    bool ok = false;

    bx_ln_parent_dir_dup(stripped, &parent_path);

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

static bool bx_ln_prepare_destination_for_replace(const char* destination_path, const struct bx_backup_params* backup_params, bool backup_enabled, struct bx_diag_ctx* diag) {
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

    if (backup_enabled) {
        enum bx_backup_create_result backup_result = bx_backup_create(destination_path, backup_params, diag, NULL);
        return backup_result != BX_BACKUP_CREATE_FAILED;
    }

    return bx_ln_remove_destination_for_force(destination_path, diag);
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

static bool bx_ln_prompt_replace(const struct bx_ln_options* options, const char* destination_path) {
    size_t prompt_len = strlen(options->progname) + strlen(destination_path) + sizeof(": replace ''? ");
    char* prompt = xmalloc(prompt_len);

    snprintf(prompt, prompt_len, "%s: replace '%s'? ", options->progname, destination_path);
    bool confirmed = bx_prompt_confirm(prompt);
    free(prompt);
    return confirmed;
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

static bool bx_ln_create_link(const struct bx_ln_options* options, const struct bx_backup_params* backup_params, struct bx_diag_ctx* diag, const char* source_path, const char* destination_path) {
    int err = 0;
    bool backup_enabled = bx_args_backup_mode_enabled(backup_params->mode);

    if (!options->symbolic && !options->allow_directory_hard_links && bx_ln_path_is_directory(source_path, options->follow_symlinks)) {
        bx_diag(diag, "%s: hard link not allowed for directory", source_path);
        return false;
    }

    if (bx_ln_create_link_once(options, source_path, destination_path, &err)) {
        bx_ln_print_verbose_line(options, source_path, destination_path);
        return true;
    }

    if (err != EEXIST || (!options->force && !options->interactive && !backup_enabled)) {
        bx_ln_diag_create_failure(options, diag, source_path, destination_path, err);
        return false;
    }

    if (!options->symbolic && bx_ln_paths_name_same_directory_entry(source_path, destination_path)) {
        if (!options->interactive || backup_enabled) {
            bx_diag(diag, "'%s' and '%s' are the same file", source_path, destination_path);
            return false;
        }
    }

    if (options->interactive && !bx_ln_prompt_replace(options, destination_path)) {
        diag->exit_status = 1;
        return true;
    }

    if (!backup_enabled && !options->symbolic && bx_ln_hard_link_already_exists(source_path, destination_path, options->follow_symlinks)) {
        bx_ln_print_verbose_line(options, source_path, destination_path);
        return true;
    }

    if (!bx_ln_prepare_destination_for_replace(destination_path, backup_params, backup_enabled, diag)) {
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
    struct bx_backup_params backup_params;
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
    bx_backup_get_params(options.backup_mode, options.suffix, &backup_params);

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
        const char* link_source_path = source_path;
        char* relative_source_path = NULL;

        if (options.relative) {
            relative_source_path = bx_ln_make_relative_source_path(source_path, destination_path, &diag);
            if (relative_source_path == NULL) {
                free(destination_path);
                continue;
            }
            link_source_path = relative_source_path;
        }

        bx_ln_create_link(&options, &backup_params, &diag, link_source_path, destination_path);
        free(relative_source_path);
        free(destination_path);
    }

    return diag.exit_status;
}
