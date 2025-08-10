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
#include "bx/diag.h"
#include "bx/libbx.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

enum bx_readlink_mode {
    BX_READLINK_MODE_READ_SYMLINK = 0,
    BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST,
    BX_READLINK_MODE_CANONICALIZE_EXISTING,
    BX_READLINK_MODE_CANONICALIZE_MISSING,
};

struct bx_readlink_components {
    char** parts;
    size_t count;
};

struct bx_readlink_options {
    const char* progname;
    enum bx_readlink_mode mode;
    bool no_newline;
    bool zero_terminated;
    bool verbose_errors;
    bool posixly_correct;
    bool show_help;
    bool show_version;
};

static const char* bx_readlink_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "readlink";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_readlink_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Print value of a symbolic link or canonical file name\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --canonicalize\n");
    fprintf(stream, "         canonicalize by following every symlink\n");
    fprintf(stream, "         in every component of the given name recursively;\n");
    fprintf(stream, "         all but the last component must exist\n");
    fprintf(stream, "  -e, --canonicalize-existing\n");
    fprintf(stream, "         canonicalize by following every symlink\n");
    fprintf(stream, "         in every component of the given name recursively;\n");
    fprintf(stream, "         all components must exist\n");
    fprintf(stream, "  -m, --canonicalize-missing\n");
    fprintf(stream, "         canonicalize by following every symlink\n");
    fprintf(stream, "         in every component of the given name recursively,\n");
    fprintf(stream, "         without requirements on components existence\n");
    fprintf(stream, "  -n, --no-newline\n");
    fprintf(stream, "         do not output the trailing delimiter\n");
    fprintf(stream, "  -q, --quiet\n");
    fprintf(stream, "  -s, --silent\n");
    fprintf(stream, "         suppress most error messages\n");
    fprintf(stream, "         (on by default if POSIXLY_CORRECT is not set)\n");
    fprintf(stream, "  -v, --verbose\n");
    fprintf(stream, "         report error messages\n");
    fprintf(stream, "         (on by default if POSIXLY_CORRECT is set)\n");
    fprintf(stream, "  -z, --zero\n");
    fprintf(stream, "         end each output line with NUL, not newline\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static void bx_readlink_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_readlink_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_readlink_parse_options(int argc, char** argv, struct bx_readlink_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"canonicalize", no_argument, NULL, 'f'},
        {"canonicalize-existing", no_argument, NULL, 'e'},
        {"canonicalize-missing", no_argument, NULL, 'm'},
        {"no-newline", no_argument, NULL, 'n'},
        {"quiet", no_argument, NULL, 'q'},
        {"silent", no_argument, NULL, 's'},
        {"verbose", no_argument, NULL, 'v'},
        {"zero", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_readlink_progname((argc > 0) ? argv[0] : NULL);
    options->mode = BX_READLINK_MODE_READ_SYMLINK;
    options->posixly_correct = (getenv("POSIXLY_CORRECT") != NULL);
    options->verbose_errors = options->posixly_correct;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+efmnqsvz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'f':
                options->mode = BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST;
                break;
            case 'e':
                options->mode = BX_READLINK_MODE_CANONICALIZE_EXISTING;
                break;
            case 'm':
                options->mode = BX_READLINK_MODE_CANONICALIZE_MISSING;
                break;
            case 'n':
                options->no_newline = true;
                break;
            case 'q':
            case 's':
                if (!options->posixly_correct) {
                    options->verbose_errors = false;
                }
                break;
            case 'v':
                if (!options->posixly_correct) {
                    options->verbose_errors = true;
                }
                break;
            case 'z':
                options->zero_terminated = true;
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

static void bx_readlink_components_push(struct bx_readlink_components* components, const char* part) {
    components->parts = xrealloc(components->parts, (components->count + 1u) * sizeof(*components->parts));
    components->parts[components->count++] = xstrdup(part);
}

static void bx_readlink_components_pop(struct bx_readlink_components* components) {
    if (components->count == 0u) {
        return;
    }

    free(components->parts[components->count - 1u]);
    components->count--;
}

static void bx_readlink_components_free(struct bx_readlink_components* components) {
    for (size_t i = 0; i < components->count; i++) {
        free(components->parts[i]);
    }

    free(components->parts);
    components->parts = NULL;
    components->count = 0u;
}

static void bx_readlink_components_clear(struct bx_readlink_components* components) {
    bx_readlink_components_free(components);
}

static bool bx_readlink_components_shift(struct bx_readlink_components* components, char** part_out) {
    if (components->count == 0u) {
        return false;
    }

    char* part = components->parts[0];
    if (components->count > 1u) {
        memmove(components->parts, components->parts + 1u, (components->count - 1u) * sizeof(*components->parts));
    }
    components->count--;
    *part_out = part;
    return true;
}

static void bx_readlink_components_append_raw(struct bx_readlink_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        bx_readlink_components_push(components, token);
    }

    free(copy);
}

static void bx_readlink_components_append_normalized_part(struct bx_readlink_components* components, const char* part) {
    if (strcmp(part, ".") == 0 || part[0] == '\0') {
        return;
    }
    if (strcmp(part, "..") == 0) {
        bx_readlink_components_pop(components);
        return;
    }

    bx_readlink_components_push(components, part);
}

static void bx_readlink_components_append_normalized_and_clear(struct bx_readlink_components* components, struct bx_readlink_components* remainder) {
    for (size_t i = 0; i < remainder->count; i++) {
        bx_readlink_components_append_normalized_part(components, remainder->parts[i]);
    }
    bx_readlink_components_clear(remainder);
}

static void bx_readlink_components_prepend_path(struct bx_readlink_components* components, const char* path) {
    struct bx_readlink_components head = {0};
    bx_readlink_components_append_raw(&head, path);

    if (head.count == 0u) {
        bx_readlink_components_free(&head);
        return;
    }

    char** merged = xmalloc((head.count + components->count) * sizeof(*merged));
    memcpy(merged, head.parts, head.count * sizeof(*merged));
    if (components->count > 0u) {
        memcpy(merged + head.count, components->parts, components->count * sizeof(*merged));
    }

    free(head.parts);
    free(components->parts);
    components->parts = merged;
    components->count += head.count;
}

static char* bx_readlink_components_to_absolute_path(const struct bx_readlink_components* components, size_t count) {
    if (count == 0u) {
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
    size_t pos = 0u;
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

static char* bx_readlink_getcwd_dup(void) {
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

static char* bx_readlink_make_absolute_input(const char* path) {
    if (path[0] == '/') {
        return xstrdup(path);
    }

    char* cwd = bx_readlink_getcwd_dup();
    if (cwd == NULL) {
        return NULL;
    }

    char* absolute = bx_path_join(cwd, path);
    free(cwd);
    return absolute;
}

static bool bx_readlink_readlink_alloc(const char* path, char** target_out) {
    size_t buffer_size = 128u;

    while (true) {
        char* buffer = xmalloc(buffer_size + 1u);
        ssize_t len = readlink(path, buffer, buffer_size);

        if (len < 0) {
            free(buffer);
            return false;
        }

        if ((size_t)len < buffer_size) {
            buffer[(size_t)len] = '\0';
            *target_out = buffer;
            return true;
        }

        free(buffer);
        if (buffer_size > ((size_t)-1) / 2u) {
            errno = ENAMETOOLONG;
            return false;
        }
        buffer_size *= 2u;
    }
}

static bool bx_readlink_has_non_root_trailing_slash(const char* path) {
    size_t len = strlen(path);
    if (len == 0u) {
        return false;
    }

    size_t trimmed = len;
    while (trimmed > 0u && path[trimmed - 1u] == '/') {
        trimmed--;
    }

    return trimmed > 0u && trimmed < len;
}

static char* bx_readlink_canonicalize_path(const char* path, enum bx_readlink_mode mode) {
    struct bx_readlink_components pending = {0};
    struct bx_readlink_components resolved = {0};
    char* absolute_input = NULL;
    char* output = NULL;
    bool require_directory_if_existing = false;
    size_t symlink_expansions = 0u;

    if (path == NULL || path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    absolute_input = bx_readlink_make_absolute_input(path);
    if (absolute_input == NULL) {
        return NULL;
    }

    require_directory_if_existing = bx_readlink_has_non_root_trailing_slash(path);
    bx_readlink_components_append_raw(&pending, absolute_input);
    free(absolute_input);

    while (pending.count > 0u) {
        char* part = NULL;
        char* base = NULL;
        char* candidate = NULL;
        struct stat st;
        bool has_more = false;
        int saved_errno = 0;

        (void)bx_readlink_components_shift(&pending, &part);
        has_more = (pending.count > 0u);

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            free(part);
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_readlink_components_pop(&resolved);
            free(part);
            continue;
        }

        base = bx_readlink_components_to_absolute_path(&resolved, resolved.count);
        candidate = bx_path_join(base, part);
        free(base);

        if (lstat(candidate, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                char* target = NULL;

                if (symlink_expansions >= 40u) {
                    if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
                        bx_readlink_components_append_normalized_part(&resolved, part);
                        bx_readlink_components_append_normalized_and_clear(&resolved, &pending);
                        free(candidate);
                        free(part);
                        break;
                    }

                    free(candidate);
                    free(part);
                    errno = ELOOP;
                    goto fail;
                }

                symlink_expansions++;
                if (!bx_readlink_readlink_alloc(candidate, &target)) {
                    saved_errno = errno;
                    free(candidate);
                    free(part);
                    errno = saved_errno;
                    goto fail;
                }

                if (target[0] == '/') {
                    bx_readlink_components_clear(&resolved);
                }
                bx_readlink_components_prepend_path(&pending, target);

                free(target);
                free(candidate);
                free(part);
                continue;
            }

            if (has_more && !S_ISDIR(st.st_mode)) {
                if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
                    bx_readlink_components_append_normalized_part(&resolved, part);
                    bx_readlink_components_append_normalized_and_clear(&resolved, &pending);
                    free(candidate);
                    free(part);
                    break;
                }

                free(candidate);
                free(part);
                errno = ENOTDIR;
                goto fail;
            }

            bx_readlink_components_append_normalized_part(&resolved, part);
            free(candidate);
            free(part);
            continue;
        }

        saved_errno = errno;
        if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
            bx_readlink_components_append_normalized_part(&resolved, part);
            bx_readlink_components_append_normalized_and_clear(&resolved, &pending);
            free(candidate);
            free(part);
            break;
        }

        if (mode == BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST && !has_more && saved_errno == ENOENT) {
            bx_readlink_components_append_normalized_part(&resolved, part);
            free(candidate);
            free(part);
            break;
        }

        free(candidate);
        free(part);
        errno = saved_errno;
        goto fail;
    }

    output = bx_readlink_components_to_absolute_path(&resolved, resolved.count);

    if (require_directory_if_existing && mode != BX_READLINK_MODE_CANONICALIZE_MISSING) {
        struct stat st;

        if (stat(output, &st) != 0) {
            int saved_errno = errno;
            if (!(mode == BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST && saved_errno == ENOENT)) {
                free(output);
                output = NULL;
                errno = saved_errno;
                goto fail;
            }
        }
        else if (!S_ISDIR(st.st_mode)) {
            free(output);
            output = NULL;
            errno = ENOTDIR;
            goto fail;
        }
    }

    bx_readlink_components_free(&pending);
    bx_readlink_components_free(&resolved);
    return output;

fail:
    bx_readlink_components_free(&pending);
    bx_readlink_components_free(&resolved);
    return NULL;
}

static void bx_readlink_report_path_error(struct bx_diag_ctx* diag, bool verbose_errors, const char* path, const char* action, int errnum) {
    if (!verbose_errors) {
        diag->exit_status = 1;
        return;
    }
    bx_diag(diag, "cannot %s '%s': %s", action, path, strerror(errnum));
}

static bool bx_readlink_emit_target(const char* target, bool no_newline, bool zero_terminated, struct bx_diag_ctx* diag) {
    if (fputs(target, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (!no_newline && fputc(zero_terminated ? '\0' : '\n', stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_readlink_process_operand(const char* operand, const struct bx_readlink_options* options, bool no_newline, struct bx_diag_ctx* diag) {
    char* output = NULL;
    const char* action = NULL;

    if (options->mode == BX_READLINK_MODE_READ_SYMLINK) {
        action = "read link";
        if (!bx_readlink_readlink_alloc(operand, &output)) {
            int saved_errno = errno;
            bx_readlink_report_path_error(diag, options->verbose_errors, operand, action, saved_errno);
            return false;
        }
    }
    else {
        action = "canonicalize";
        output = bx_readlink_canonicalize_path(operand, options->mode);
        if (output == NULL) {
            int saved_errno = errno;
            bx_readlink_report_path_error(diag, options->verbose_errors, operand, action, saved_errno);
            return false;
        }
    }

    bool emitted = bx_readlink_emit_target(output, no_newline, options->zero_terminated, diag);
    free(output);
    return emitted;
}

int bx_readlink_main(int argc, char** argv) {
    struct bx_readlink_options options;
    struct bx_diag_ctx diag = {
        .progname = "readlink",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_readlink_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_readlink_print_try_help(options.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_readlink_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_readlink_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        bx_readlink_print_try_help(options.progname);
        return diag.exit_status;
    }

    bool no_newline = options.no_newline;
    if (no_newline && operand_count > 1) {
        fprintf(stderr, "%s: ignoring --no-newline with multiple arguments\n", options.progname);
        no_newline = false;
    }

    char** operands = argv + first_operand;
    for (int i = 0; i < operand_count; i++) {
        if (!bx_readlink_process_operand(operands[i], &options, no_newline, &diag)) {
            continue;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
