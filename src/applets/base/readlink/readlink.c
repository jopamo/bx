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
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

enum bx_readlink_mode {
    BX_READLINK_MODE_READ_SYMLINK = 0,
    BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST,
    BX_READLINK_MODE_CANONICALIZE_EXISTING,
    BX_READLINK_MODE_CANONICALIZE_MISSING,
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
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "readlink");
    options->mode = BX_READLINK_MODE_READ_SYMLINK;
    options->posixly_correct = (getenv("POSIXLY_CORRECT") != NULL);
    options->verbose_errors = options->posixly_correct;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+efmnqsvz", long_options, &option_index);
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
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static void bx_readlink_components_append_normalized_and_clear(struct bx_path_components* components, struct bx_path_components* remainder) {
    for (size_t i = 0; i < remainder->count; i++) {
        bx_path_components_append_normalized_part(components, remainder->parts[i]);
    }
    bx_path_components_free(remainder);
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
    struct bx_path_components pending = {0};
    struct bx_path_components resolved = {0};
    char* absolute_input = NULL;
    char* output = NULL;
    bool require_directory_if_existing = false;
    size_t symlink_expansions = 0u;

    if (path == NULL || path[0] == '\0') {
        errno = ENOENT;
        return NULL;
    }

    absolute_input = bx_path_make_absolute_dup(path);
    if (absolute_input == NULL) {
        return NULL;
    }

    require_directory_if_existing = bx_readlink_has_non_root_trailing_slash(path);
    bx_path_components_append_raw(&pending, absolute_input);
    free(absolute_input);

    while (pending.count > 0u) {
        char* part = NULL;
        char* base = NULL;
        char* candidate = NULL;
        struct stat st;
        bool has_more = false;
        int saved_errno = 0;

        (void)bx_path_components_shift(&pending, &part);
        has_more = (pending.count > 0u);

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            free(part);
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_path_components_pop(&resolved);
            free(part);
            continue;
        }

        base = bx_path_components_to_absolute_path(&resolved, resolved.count);
        candidate = bx_path_join(base, part);
        free(base);

        if (lstat(candidate, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                char* target = NULL;

                if (symlink_expansions >= 40u) {
                    if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
                        bx_path_components_append_normalized_part(&resolved, part);
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
                    bx_path_components_free(&resolved);
                }
                bx_path_components_prepend_raw_path(&pending, target);

                free(target);
                free(candidate);
                free(part);
                continue;
            }

            if (has_more && !S_ISDIR(st.st_mode)) {
                if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
                    bx_path_components_append_normalized_part(&resolved, part);
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

            bx_path_components_append_normalized_part(&resolved, part);
            free(candidate);
            free(part);
            continue;
        }

        saved_errno = errno;
        if (mode == BX_READLINK_MODE_CANONICALIZE_MISSING) {
            bx_path_components_append_normalized_part(&resolved, part);
            bx_readlink_components_append_normalized_and_clear(&resolved, &pending);
            free(candidate);
            free(part);
            break;
        }

        if (mode == BX_READLINK_MODE_CANONICALIZE_EXISTING_BUT_LAST && !has_more && saved_errno == ENOENT) {
            bx_path_components_append_normalized_part(&resolved, part);
            free(candidate);
            free(part);
            break;
        }

        free(candidate);
        free(part);
        errno = saved_errno;
        goto fail;
    }

    output = bx_path_components_to_absolute_path(&resolved, resolved.count);

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

    bx_path_components_free(&pending);
    bx_path_components_free(&resolved);
    return output;

fail:
    bx_path_components_free(&pending);
    bx_path_components_free(&resolved);
    return NULL;
}

static void bx_readlink_report_path_error(struct bx_diag_ctx* diag, bool verbose_errors, const char* path, const char* action, int errnum) {
    if (!verbose_errors) {
        diag->exit_status = 1;
        return;
    }
    bx_diag(diag, "cannot %s '%s': %s", action, path, strerror(errnum));
}

static bool bx_readlink_emit_target(struct bx_line_writer* writer, const char* target, bool no_newline, bool zero_terminated, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_puts(writer, target)) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (!no_newline && !bx_line_writer_putc(writer, zero_terminated ? '\0' : '\n')) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_readlink_process_operand(const char* operand,
                                        const struct bx_readlink_options* options,
                                        bool no_newline,
                                        struct bx_diag_ctx* diag,
                                        struct bx_line_writer* writer) {
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

    bool emitted = bx_readlink_emit_target(writer, output, no_newline, options->zero_terminated, diag);
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
        bx_cli_print_try_help(options.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_readlink_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        bx_cli_print_try_help(options.progname);
        return diag.exit_status;
    }

    bool no_newline = options.no_newline;
    if (no_newline && operand_count > 1) {
        fprintf(stderr, "%s: ignoring --no-newline with multiple arguments\n", options.progname);
        no_newline = false;
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    char** operands = argv + first_operand;
    for (int i = 0; i < operand_count; i++) {
        if (!bx_readlink_process_operand(operands[i], &options, no_newline, &diag, &writer)) {
            continue;
        }
    }

    if (!bx_line_writer_flush(&writer)) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
