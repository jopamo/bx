#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "lib/path_ops.h"
#include "diag.h"
#include "libbx.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

enum bx_realpath_canonicalization_mode {
    BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST = 0,
    BX_REALPATH_CANONICALIZE_EXISTING,
    BX_REALPATH_CANONICALIZE_MISSING,
};

enum bx_realpath_symlink_mode {
    BX_REALPATH_SYMLINK_PHYSICAL = 0,
    BX_REALPATH_SYMLINK_LOGICAL,
    BX_REALPATH_SYMLINK_NONE,
};

struct bx_realpath_components {
    char** parts;
    size_t count;
};

struct bx_realpath_options {
    const char* progname;
    enum bx_realpath_canonicalization_mode canonicalization_mode;
    enum bx_realpath_symlink_mode symlink_mode;
    const char* relative_to_arg;
    const char* relative_base_arg;
    bool quiet;
    bool zero;
    bool show_help;
    bool show_version;
};

static const char* bx_realpath_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "realpath";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_realpath_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Print the resolved absolute file name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -E, --canonicalize           all but the last component must exist (default)\n");
    fprintf(stream, "  -e, --canonicalize-existing  all components of the path must exist\n");
    fprintf(stream, "  -m, --canonicalize-missing   no path components need exist\n");
    fprintf(stream, "  -L, --logical                resolve '..' components before symlinks\n");
    fprintf(stream, "  -P, --physical               resolve symlinks as encountered (default)\n");
    fprintf(stream, "  -q, --quiet    suppress most error messages\n");
    fprintf(stream, "      --relative-to=DIR        print the resolved path relative to DIR\n");
    fprintf(stream, "      --relative-base=DIR      print absolute paths unless below DIR\n");
    fprintf(stream, "  -s, --strip, --no-symlinks   don't expand symlinks\n");
    fprintf(stream, "  -z, --zero     end each output line with NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_realpath_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_realpath_parse_options(int argc, char** argv, struct bx_realpath_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"canonicalize", no_argument, NULL, 'E'},
        {"canonicalize-existing", no_argument, NULL, 'e'},
        {"canonicalize-missing", no_argument, NULL, 'm'},
        {"logical", no_argument, NULL, 'L'},
        {"physical", no_argument, NULL, 'P'},
        {"quiet", no_argument, NULL, 'q'},
        {"relative-to", required_argument, NULL, 3},
        {"relative-base", required_argument, NULL, 4},
        {"strip", no_argument, NULL, 's'},
        {"no-symlinks", no_argument, NULL, 's'},
        {"zero", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_realpath_progname((argc > 0) ? argv[0] : NULL);
    options->canonicalization_mode = BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST;
    options->symlink_mode = BX_REALPATH_SYMLINK_PHYSICAL;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+EeLmPqsz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'E':
                options->canonicalization_mode = BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST;
                break;
            case 'e':
                options->canonicalization_mode = BX_REALPATH_CANONICALIZE_EXISTING;
                break;
            case 'm':
                options->canonicalization_mode = BX_REALPATH_CANONICALIZE_MISSING;
                break;
            case 'L':
                options->symlink_mode = BX_REALPATH_SYMLINK_LOGICAL;
                break;
            case 'P':
                options->symlink_mode = BX_REALPATH_SYMLINK_PHYSICAL;
                break;
            case 'q':
                options->quiet = true;
                break;
            case 3:
                options->relative_to_arg = optarg;
                break;
            case 4:
                options->relative_base_arg = optarg;
                break;
            case 's':
                options->symlink_mode = BX_REALPATH_SYMLINK_NONE;
                break;
            case 'z':
                options->zero = true;
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

static void bx_realpath_components_push(struct bx_realpath_components* components, const char* part) {
    components->parts = xrealloc(components->parts, sizeof(*components->parts) * (components->count + 1u));
    components->parts[components->count++] = xstrdup(part);
}

static void bx_realpath_components_pop(struct bx_realpath_components* components) {
    if (components->count == 0) {
        return;
    }

    free(components->parts[components->count - 1u]);
    components->count--;
}

static void bx_realpath_components_free(struct bx_realpath_components* components) {
    for (size_t i = 0; i < components->count; i++) {
        free(components->parts[i]);
    }

    free(components->parts);
    components->parts = NULL;
    components->count = 0;
}

static void bx_realpath_components_clear(struct bx_realpath_components* components) {
    bx_realpath_components_free(components);
}

static void bx_realpath_components_append_raw(struct bx_realpath_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        bx_realpath_components_push(components, token);
    }

    free(copy);
}

static void bx_realpath_components_insert_raw_path(struct bx_realpath_components* components, size_t index, const char* path) {
    struct bx_realpath_components inserted = {0};

    bx_realpath_components_append_raw(&inserted, path);
    if (inserted.count == 0) {
        return;
    }

    components->parts = xrealloc(components->parts, sizeof(*components->parts) * (components->count + inserted.count));
    memmove(&components->parts[index + inserted.count], &components->parts[index], sizeof(*components->parts) * (components->count - index));
    memcpy(&components->parts[index], inserted.parts, sizeof(*components->parts) * inserted.count);
    components->count += inserted.count;
    free(inserted.parts);
}

static void bx_realpath_components_append_normalized(struct bx_realpath_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        if (strcmp(token, ".") == 0 || token[0] == '\0') {
            continue;
        }
        if (strcmp(token, "..") == 0) {
            bx_realpath_components_pop(components);
            continue;
        }

        bx_realpath_components_push(components, token);
    }

    free(copy);
}

static void bx_realpath_components_append_normalized_part(struct bx_realpath_components* components, const char* part) {
    if (strcmp(part, ".") == 0 || part[0] == '\0') {
        return;
    }
    if (strcmp(part, "..") == 0) {
        bx_realpath_components_pop(components);
        return;
    }

    bx_realpath_components_push(components, part);
}

static char* bx_realpath_components_to_absolute_path(const struct bx_realpath_components* components, size_t count) {
    if (count == 0) {
        return xstrdup("/");
    }

    size_t len = 2u;
    for (size_t i = 0; i < count; i++) {
        len += strlen(components->parts[i]);
        if (i + 1u < count) {
            len++;
        }
    }

    char* path = xmalloc(len);
    size_t pos = 0;
    path[pos++] = '/';

    for (size_t i = 0; i < count; i++) {
        size_t part_len = strlen(components->parts[i]);
        memcpy(path + pos, components->parts[i], part_len);
        pos += part_len;
        if (i + 1u < count) {
            path[pos++] = '/';
        }
    }

    path[pos] = '\0';
    return path;
}

static char* bx_realpath_getcwd_dup(void) {
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

static char* bx_realpath_make_absolute_input(const char* path) {
    if (path[0] == '/') {
        return xstrdup(path);
    }

    char* cwd = bx_realpath_getcwd_dup();
    if (cwd == NULL) {
        return NULL;
    }

    char* absolute = bx_path_join(cwd, path);
    free(cwd);
    return absolute;
}

static char* bx_realpath_normalize_absolute_lexical_dup(const char* absolute_path) {
    struct bx_realpath_components components = {0};
    char* normalized;

    bx_realpath_components_append_normalized(&components, absolute_path);
    normalized = bx_realpath_components_to_absolute_path(&components, components.count);
    bx_realpath_components_free(&components);
    return normalized;
}

static bool bx_realpath_path_is_within(const char* path, const char* base) {
    size_t base_len;

    if (path == NULL || base == NULL || path[0] != '/' || base[0] != '/') {
        return false;
    }

    if (strcmp(base, "/") == 0) {
        return true;
    }

    base_len = strlen(base);
    if (strncmp(path, base, base_len) != 0) {
        return false;
    }
    return path[base_len] == '\0' || path[base_len] == '/';
}

static char* bx_realpath_relative_path_between(const char* from_abs, const char* to_abs) {
    struct bx_realpath_components from_components = {0};
    struct bx_realpath_components to_components = {0};
    size_t common = 0;
    size_t up_count;
    size_t down_count;
    size_t segment_count;
    size_t len;
    size_t pos = 0;
    char* relative;

    if (from_abs == NULL || to_abs == NULL || from_abs[0] != '/' || to_abs[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    bx_realpath_components_append_normalized(&from_components, from_abs);
    bx_realpath_components_append_normalized(&to_components, to_abs);

    while (common < from_components.count && common < to_components.count && strcmp(from_components.parts[common], to_components.parts[common]) == 0) {
        common++;
    }

    up_count = from_components.count - common;
    down_count = to_components.count - common;
    segment_count = up_count + down_count;
    if (segment_count == 0) {
        relative = xstrdup(".");
        bx_realpath_components_free(&from_components);
        bx_realpath_components_free(&to_components);
        return relative;
    }

    len = 1u;
    if (segment_count > 1u) {
        len += segment_count - 1u;
    }
    len += up_count * 2u;
    for (size_t i = common; i < to_components.count; i++) {
        len += strlen(to_components.parts[i]);
    }

    relative = xmalloc(len);
    for (size_t i = 0; i < up_count; i++) {
        if (pos > 0) {
            relative[pos++] = '/';
        }
        relative[pos++] = '.';
        relative[pos++] = '.';
    }
    for (size_t i = common; i < to_components.count; i++) {
        size_t part_len = strlen(to_components.parts[i]);
        if (pos > 0) {
            relative[pos++] = '/';
        }
        memcpy(relative + pos, to_components.parts[i], part_len);
        pos += part_len;
    }
    relative[pos] = '\0';

    bx_realpath_components_free(&from_components);
    bx_realpath_components_free(&to_components);
    return relative;
}

static char* bx_realpath_readlink_dup(const char* path) {
    size_t size = 128u;

    while (true) {
        char* buf = xmalloc(size);
        ssize_t len = readlink(path, buf, size);
        if (len < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)len < size) {
            buf[len] = '\0';
            return buf;
        }

        free(buf);
        size *= 2u;
    }
}

static char* bx_realpath_canonicalize_path_physical(const char* path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    char* absolute_input = bx_realpath_make_absolute_input(path);
    struct bx_realpath_components pending = {0};
    struct bx_realpath_components resolved_components = {0};
    char* resolved = NULL;

    if (absolute_input == NULL) {
        return NULL;
    }

    bx_realpath_components_append_raw(&pending, absolute_input);
    free(absolute_input);

    for (size_t i = 0; i < pending.count; i++) {
        const char* part = pending.parts[i];
        struct stat st;
        char* current_path;

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_realpath_components_pop(&resolved_components);
            continue;
        }

        current_path = bx_realpath_components_to_absolute_path(&resolved_components, resolved_components.count);
        char* next_path = bx_path_join(current_path, part);
        free(current_path);

        if (lstat(next_path, &st) != 0) {
            int saved_errno = errno;
            if (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING) {
                free(next_path);
                bx_realpath_components_free(&pending);
                bx_realpath_components_free(&resolved_components);
                errno = saved_errno;
                return NULL;
            }

            if (saved_errno == ENOENT || (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING && saved_errno == ENOTDIR)) {
                if (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST && i + 1u < pending.count) {
                    free(next_path);
                    bx_realpath_components_free(&pending);
                    bx_realpath_components_free(&resolved_components);
                    errno = saved_errno;
                    return NULL;
                }
                bx_realpath_components_push(&resolved_components, part);
                for (size_t j = i + 1u; j < pending.count; j++) {
                    bx_realpath_components_append_normalized_part(&resolved_components, pending.parts[j]);
                }
                resolved = bx_realpath_components_to_absolute_path(&resolved_components, resolved_components.count);
                free(next_path);
                bx_realpath_components_free(&pending);
                bx_realpath_components_free(&resolved_components);
                return resolved;
            }

            free(next_path);
            bx_realpath_components_free(&pending);
            bx_realpath_components_free(&resolved_components);
            errno = saved_errno;
            return NULL;
        }

        if (S_ISLNK(st.st_mode)) {
            char* target = bx_realpath_readlink_dup(next_path);
            free(next_path);
            if (target == NULL) {
                int saved_errno = errno;
                bx_realpath_components_free(&pending);
                bx_realpath_components_free(&resolved_components);
                errno = saved_errno;
                return NULL;
            }

            free(pending.parts[i]);
            memmove(&pending.parts[i], &pending.parts[i + 1u], sizeof(*pending.parts) * (pending.count - (i + 1u)));
            pending.count--;
            if (target[0] == '/') {
                bx_realpath_components_clear(&resolved_components);
            }
            bx_realpath_components_insert_raw_path(&pending, i, target);
            free(target);
            i--;
            continue;
        }

        bx_realpath_components_push(&resolved_components, part);
        free(next_path);
        if (i + 1u < pending.count && !S_ISDIR(st.st_mode)) {
            if (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING) {
                for (size_t j = i + 1u; j < pending.count; j++) {
                    bx_realpath_components_append_normalized_part(&resolved_components, pending.parts[j]);
                }
                resolved = bx_realpath_components_to_absolute_path(&resolved_components, resolved_components.count);
                bx_realpath_components_free(&pending);
                bx_realpath_components_free(&resolved_components);
                return resolved;
            }
            bx_realpath_components_free(&pending);
            bx_realpath_components_free(&resolved_components);
            errno = ENOTDIR;
            return NULL;
        }
    }

    resolved = bx_realpath_components_to_absolute_path(&resolved_components, resolved_components.count);
    bx_realpath_components_free(&pending);
    bx_realpath_components_free(&resolved_components);
    return resolved;
}

static bool bx_realpath_validate_no_symlinks_path(const char* normalized_path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    struct bx_realpath_components components = {0};
    char* current = xstrdup("/");

    if (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING) {
        free(current);
        return true;
    }

    bx_realpath_components_append_raw(&components, normalized_path);

    for (size_t i = 0; i < components.count; i++) {
        char* next = bx_path_join(current, components.parts[i]);
        bool is_last = (i + 1u == components.count);
        bool must_check = (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING) || !is_last;
        struct stat st;

        free(current);
        current = next;
        if (!must_check) {
            continue;
        }

        if (stat(current, &st) != 0) {
            int saved_errno = errno;
            if (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST && saved_errno == ENOENT) {
                break;
            }

            bx_realpath_components_free(&components);
            free(current);
            errno = saved_errno;
            return false;
        }

        if (!is_last && !S_ISDIR(st.st_mode)) {
            bx_realpath_components_free(&components);
            free(current);
            errno = ENOTDIR;
            return false;
        }
    }

    bx_realpath_components_free(&components);
    free(current);
    return true;
}

static char* bx_realpath_canonicalize_path_no_symlinks(const char* path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    char* absolute_input = bx_realpath_make_absolute_input(path);
    char* normalized;

    if (absolute_input == NULL) {
        return NULL;
    }

    normalized = bx_realpath_normalize_absolute_lexical_dup(absolute_input);
    free(absolute_input);
    if (normalized == NULL) {
        return NULL;
    }

    if (!bx_realpath_validate_no_symlinks_path(normalized, canonicalization_mode)) {
        free(normalized);
        return NULL;
    }

    return normalized;
}

static bool bx_realpath_validate_logical_input_path(const char* absolute_input, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    struct bx_realpath_components input_components = {0};
    struct bx_realpath_components logical_components = {0};

    if (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING) {
        return true;
    }

    bx_realpath_components_append_raw(&input_components, absolute_input);

    for (size_t i = 0; i < input_components.count; i++) {
        const char* part = input_components.parts[i];
        char* current_path;
        struct stat st;
        bool must_check;

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_realpath_components_pop(&logical_components);
            continue;
        }

        bx_realpath_components_push(&logical_components, part);
        must_check = (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING) || (i + 1u < input_components.count);
        if (!must_check) {
            continue;
        }

        current_path = bx_realpath_components_to_absolute_path(&logical_components, logical_components.count);
        if (stat(current_path, &st) != 0) {
            int saved_errno = errno;
            free(current_path);
            bx_realpath_components_free(&input_components);
            bx_realpath_components_free(&logical_components);
            errno = saved_errno;
            return false;
        }
        free(current_path);
    }

    bx_realpath_components_free(&input_components);
    bx_realpath_components_free(&logical_components);
    return true;
}

static char* bx_realpath_canonicalize_path(const char* path, enum bx_realpath_canonicalization_mode canonicalization_mode, enum bx_realpath_symlink_mode symlink_mode) {
    char* absolute_input = NULL;
    char* normalized_input = NULL;
    char* resolved;

    if (path == NULL || path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    switch (symlink_mode) {
        case BX_REALPATH_SYMLINK_PHYSICAL:
            return bx_realpath_canonicalize_path_physical(path, canonicalization_mode);
        case BX_REALPATH_SYMLINK_LOGICAL:
            absolute_input = bx_realpath_make_absolute_input(path);
            if (absolute_input == NULL) {
                return NULL;
            }

            if (!bx_realpath_validate_logical_input_path(absolute_input, canonicalization_mode)) {
                int saved_errno = errno;
                free(absolute_input);
                errno = saved_errno;
                return NULL;
            }

            normalized_input = bx_realpath_normalize_absolute_lexical_dup(absolute_input);
            free(absolute_input);
            if (normalized_input == NULL) {
                return NULL;
            }

            resolved = bx_realpath_canonicalize_path_physical(normalized_input, canonicalization_mode);
            free(normalized_input);
            return resolved;
        case BX_REALPATH_SYMLINK_NONE:
            return bx_realpath_canonicalize_path_no_symlinks(path, canonicalization_mode);
        default:
            errno = EINVAL;
            return NULL;
    }
}

static char* bx_realpath_format_output_path(const char* resolved_path, const char* relative_to, const char* relative_base) {
    if (relative_to == NULL) {
        return xstrdup(resolved_path);
    }

    if (relative_base != NULL && !bx_realpath_path_is_within(resolved_path, relative_base)) {
        return xstrdup(resolved_path);
    }

    return bx_realpath_relative_path_between(relative_to, resolved_path);
}

static bool bx_realpath_emit_path(const char* path, bool zero, struct bx_diag_ctx* diag) {
    int delimiter = zero ? '\0' : '\n';

    if (fputs(path, stdout) == EOF || fputc(delimiter, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_realpath_process_path(const char* path,
                                     enum bx_realpath_canonicalization_mode canonicalization_mode,
                                     enum bx_realpath_symlink_mode symlink_mode,
                                     const char* relative_to,
                                     const char* relative_base,
                                     bool quiet,
                                     bool zero,
                                     struct bx_diag_ctx* diag) {
    char* resolved = bx_realpath_canonicalize_path(path, canonicalization_mode, symlink_mode);
    char* output_path = NULL;
    bool ok;

    if (resolved == NULL) {
        if (quiet) {
            diag->exit_status = 1;
        }
        else {
            bx_diag(diag, "cannot resolve '%s': %s", path, strerror(errno));
        }
        return false;
    }

    output_path = bx_realpath_format_output_path(resolved, relative_to, relative_base);
    if (output_path == NULL) {
        if (quiet) {
            diag->exit_status = 1;
        }
        else {
            bx_diag(diag, "cannot format '%s': %s", path, strerror(errno));
        }
        free(resolved);
        return false;
    }

    ok = bx_realpath_emit_path(output_path, zero, diag);
    free(output_path);
    free(resolved);
    return ok;
}

int bx_realpath_main(int argc, char** argv) {
    struct bx_realpath_options options;
    struct bx_diag_ctx diag = {
        .progname = "realpath",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;
    char* relative_to = NULL;
    char* relative_base = NULL;

    if (!bx_realpath_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_realpath_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_realpath_print_version(options.progname);
        return 0;
    }

    if (options.relative_to_arg != NULL) {
        relative_to = bx_realpath_canonicalize_path(options.relative_to_arg, options.canonicalization_mode, options.symlink_mode);
        if (relative_to == NULL) {
            bx_diag(&diag, "cannot resolve '%s': %s", options.relative_to_arg, strerror(errno));
            return diag.exit_status;
        }
    }

    if (options.relative_base_arg != NULL) {
        relative_base = bx_realpath_canonicalize_path(options.relative_base_arg, options.canonicalization_mode, options.symlink_mode);
        if (relative_base == NULL) {
            bx_diag(&diag, "cannot resolve '%s': %s", options.relative_base_arg, strerror(errno));
            free(relative_to);
            return diag.exit_status;
        }

        if (relative_to == NULL) {
            relative_to = xstrdup(relative_base);
        }
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        free(relative_base);
        free(relative_to);
        return diag.exit_status;
    }

    char** operands = argv + first_operand;
    for (int i = 0; i < operand_count; i++) {
        if (!bx_realpath_process_path(operands[i], options.canonicalization_mode, options.symlink_mode, relative_to, relative_base, options.quiet, options.zero, &diag)) {
            continue;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    free(relative_base);
    free(relative_to);

    return diag.exit_status;
}
