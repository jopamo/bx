#include <errno.h>
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
#include "lib/line_writer.h"
#include "lib/option_scan.h"

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

enum {
    BX_REALPATH_OPT_HELP = 256,
    BX_REALPATH_OPT_VERSION,
    BX_REALPATH_OPT_RELATIVE_TO,
    BX_REALPATH_OPT_RELATIVE_BASE,
};

struct bx_realpath_options {
    enum bx_realpath_canonicalization_mode canonicalization_mode;
    enum bx_realpath_symlink_mode symlink_mode;
    const char* relative_to_arg;
    const char* relative_base_arg;
    bool quiet;
    bool zero;
};

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

static char* bx_realpath_canonicalize_path_physical(const char* path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    char* absolute_input = bx_path_make_absolute_dup(path);
    struct bx_path_components pending = {0};
    struct bx_path_components resolved_components = {0};
    char* resolved = NULL;
    size_t symlink_expansions = 0;
    int error = 0;

    if (absolute_input == NULL) {
        return NULL;
    }

    bx_path_components_append_raw(&pending, absolute_input);
    free(absolute_input);

    size_t i = 0;
    for (; i < pending.count; i++) {
        const char* part = pending.parts[i];
        struct stat st;
        char* current_path;

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_path_components_pop(&resolved_components);
            continue;
        }

        current_path = bx_path_components_to_absolute_path(&resolved_components, resolved_components.count);
        char* next_path = bx_path_join(current_path, part);
        free(current_path);

        if (lstat(next_path, &st) != 0) {
            error = errno;
            free(next_path);
            if ((canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING &&
                 (error == ENOENT || error == ENOTDIR)) ||
                (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST &&
                 error == ENOENT && i + 1u == pending.count)) {
                error = 0;
            }
            break;
        }

        if (S_ISLNK(st.st_mode)) {
            if (symlink_expansions == BX_PATH_SYMLINK_LIMIT) {
                free(next_path);
                if (canonicalization_mode != BX_REALPATH_CANONICALIZE_MISSING)
                    error = ELOOP;
                break;
            }
            symlink_expansions++;
            char* target = bx_path_readlink_dup(next_path);
            if (!target)
                error = errno;
            free(next_path);
            if (!target)
                break;

            free(pending.parts[i]);
            memmove(&pending.parts[i], &pending.parts[i + 1u], sizeof(*pending.parts) * (pending.count - (i + 1u)));
            pending.count--;
            if (target[0] == '/') {
                bx_path_components_free(&resolved_components);
            }
            bx_path_components_insert_raw_path(&pending, i, target);
            free(target);
            i--;
            continue;
        }

        bx_path_components_push_dup(&resolved_components, part);
        free(next_path);
        if (i + 1u < pending.count && !S_ISDIR(st.st_mode)) {
            if (canonicalization_mode != BX_REALPATH_CANONICALIZE_MISSING)
                error = ENOTDIR;
            i++;
            break;
        }
    }

    if (error == 0) {
        /* A permissive mode can finish the unresolved suffix lexically. */
        for (; i < pending.count; i++)
            bx_path_components_append_normalized_part(&resolved_components, pending.parts[i]);
        resolved = bx_path_components_to_absolute_path(&resolved_components, resolved_components.count);
    }
    bx_path_components_free(&pending);
    bx_path_components_free(&resolved_components);
    if (error != 0)
        errno = error;
    return resolved;
}

static bool bx_realpath_validate_no_symlinks_path(const char* normalized_path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    struct bx_path_components components = {0};
    char* current = xstrdup("/");

    if (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING) {
        free(current);
        return true;
    }

    bx_path_components_append_raw(&components, normalized_path);

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

            bx_path_components_free(&components);
            free(current);
            errno = saved_errno;
            return false;
        }

        if (!is_last && !S_ISDIR(st.st_mode)) {
            bx_path_components_free(&components);
            free(current);
            errno = ENOTDIR;
            return false;
        }
    }

    bx_path_components_free(&components);
    free(current);
    return true;
}

static char* bx_realpath_canonicalize_path_no_symlinks(const char* path, enum bx_realpath_canonicalization_mode canonicalization_mode) {
    char* absolute_input = bx_path_make_absolute_dup(path);
    char* normalized;

    if (absolute_input == NULL) {
        return NULL;
    }

    normalized = bx_path_normalize_absolute_lexical_dup(absolute_input);
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
    struct bx_path_components input_components = {0};
    struct bx_path_components logical_components = {0};

    if (canonicalization_mode == BX_REALPATH_CANONICALIZE_MISSING) {
        return true;
    }

    bx_path_components_append_raw(&input_components, absolute_input);

    for (size_t i = 0; i < input_components.count; i++) {
        const char* part = input_components.parts[i];
        char* current_path;
        struct stat st;
        bool must_check;

        if (strcmp(part, ".") == 0 || part[0] == '\0') {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            bx_path_components_pop(&logical_components);
            continue;
        }

        bx_path_components_push_dup(&logical_components, part);
        must_check = (canonicalization_mode == BX_REALPATH_CANONICALIZE_EXISTING) || (i + 1u < input_components.count);
        if (!must_check) {
            continue;
        }

        current_path = bx_path_components_to_absolute_path(&logical_components, logical_components.count);
        if (stat(current_path, &st) != 0) {
            int saved_errno = errno;
            free(current_path);
            bx_path_components_free(&input_components);
            bx_path_components_free(&logical_components);
            errno = saved_errno;
            return false;
        }
        free(current_path);
    }

    bx_path_components_free(&input_components);
    bx_path_components_free(&logical_components);
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
            absolute_input = bx_path_make_absolute_dup(path);
            if (absolute_input == NULL) {
                return NULL;
            }

            if (!bx_realpath_validate_logical_input_path(absolute_input, canonicalization_mode)) {
                int saved_errno = errno;
                free(absolute_input);
                errno = saved_errno;
                return NULL;
            }

            normalized_input = bx_path_normalize_absolute_lexical_dup(absolute_input);
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

    if (relative_base != NULL && !bx_path_is_within(resolved_path, relative_base)) {
        return xstrdup(resolved_path);
    }

    return bx_path_relative_path_between(relative_to, resolved_path);
}

static bool bx_realpath_emit_path(struct bx_line_writer* writer, const char* path, bool zero, struct bx_diag_ctx* diag) {
    char delimiter = zero ? '\0' : '\n';

    if (!bx_line_writer_puts(writer, path) || !bx_line_writer_putc(writer, delimiter)) {
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
                                     struct bx_diag_ctx* diag,
                                     struct bx_line_writer* writer) {
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

    ok = bx_realpath_emit_path(writer, output_path, zero, diag);
    free(output_path);
    free(resolved);
    return ok;
}

int bx_realpath_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'E', 'E', "canonicalize", BX_OPTION_NO_VALUE},
        {'e', 'e', "canonicalize-existing", BX_OPTION_NO_VALUE},
        {'m', 'm', "canonicalize-missing", BX_OPTION_NO_VALUE},
        {'L', 'L', "logical", BX_OPTION_NO_VALUE},
        {'P', 'P', "physical", BX_OPTION_NO_VALUE},
        {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
        {BX_REALPATH_OPT_RELATIVE_TO, 0, "relative-to", BX_OPTION_REQUIRED_VALUE},
        {BX_REALPATH_OPT_RELATIVE_BASE, 0, "relative-base", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "strip", BX_OPTION_NO_VALUE},
        {'s', 0, "no-symlinks", BX_OPTION_NO_VALUE},
        {'z', 'z', "zero", BX_OPTION_NO_VALUE},
        {BX_REALPATH_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_REALPATH_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    struct bx_realpath_options options = {0};
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "realpath"),
    };
    char* relative_to = NULL;
    char* relative_base = NULL;
    const char** operands = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*operands)) : NULL;
    if (argc > 1 && operands == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t operand_count = 0;
    size_t first = argc > 0 ? 1u : 0u;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, first, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            operands[operand_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'E': options.canonicalization_mode = BX_REALPATH_CANONICALIZE_EXISTING_BUT_LAST; break;
            case 'e': options.canonicalization_mode = BX_REALPATH_CANONICALIZE_EXISTING; break;
            case 'm': options.canonicalization_mode = BX_REALPATH_CANONICALIZE_MISSING; break;
            case 'L': options.symlink_mode = BX_REALPATH_SYMLINK_LOGICAL; break;
            case 'P': options.symlink_mode = BX_REALPATH_SYMLINK_PHYSICAL; break;
            case 's': options.symlink_mode = BX_REALPATH_SYMLINK_NONE; break;
            case 'q': options.quiet = true; break;
            case 'z': options.zero = true; break;
            case BX_REALPATH_OPT_RELATIVE_TO: options.relative_to_arg = option.value; break;
            case BX_REALPATH_OPT_RELATIVE_BASE: options.relative_base_arg = option.value; break;
            case BX_REALPATH_OPT_HELP:
                bx_realpath_print_help(stdout, diag.progname);
                goto done;
            case BX_REALPATH_OPT_VERSION:
                bx_cli_print_version(diag.progname);
                goto done;
        }
    }

    if (operand_count == 0) {
        bx_diag(&diag, "missing operand");
        goto done;
    }
    if ((options.relative_to_arg && !options.relative_to_arg[0]) ||
        (options.relative_base_arg && !options.relative_base_arg[0])) {
        bx_diag(&diag, "cannot resolve '': %s", strerror(ENOENT));
        goto done;
    }

    if (options.relative_to_arg != NULL) {
        relative_to = bx_realpath_canonicalize_path(options.relative_to_arg, options.canonicalization_mode, options.symlink_mode);
        if (relative_to == NULL) {
            bx_diag(&diag, "cannot resolve '%s': %s", options.relative_to_arg, strerror(errno));
            goto done;
        }
    }

    if (options.relative_base_arg != NULL) {
        relative_base = bx_realpath_canonicalize_path(options.relative_base_arg, options.canonicalization_mode, options.symlink_mode);
        if (relative_base == NULL) {
            bx_diag(&diag, "cannot resolve '%s': %s", options.relative_base_arg, strerror(errno));
            goto done;
        }

        if (relative_to == NULL) {
            relative_to = xstrdup(relative_base);
        }
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    for (size_t i = 0; i < operand_count; i++) {
        bx_realpath_process_path(operands[i], options.canonicalization_mode, options.symlink_mode,
                                 relative_to, relative_base, options.quiet, options.zero, &diag, &writer);
    }

    if (!bx_line_writer_flush(&writer)) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

done:
    free(relative_base);
    free(relative_to);
    free(operands);

    return diag.exit_status;
}
