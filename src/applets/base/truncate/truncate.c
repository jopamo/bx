#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"

enum bx_truncate_size_mode {
    BX_TRUNCATE_SIZE_SET = 0,
    BX_TRUNCATE_SIZE_INCREASE,
    BX_TRUNCATE_SIZE_DECREASE,
    BX_TRUNCATE_SIZE_AT_MOST,
    BX_TRUNCATE_SIZE_AT_LEAST,
    BX_TRUNCATE_SIZE_ROUND_DOWN,
    BX_TRUNCATE_SIZE_ROUND_UP,
};

struct bx_truncate_size_spec {
    enum bx_truncate_size_mode mode;
    uintmax_t value;
};

enum {
    BX_TRUNCATE_OPT_HELP = 256,
    BX_TRUNCATE_OPT_VERSION,
};

struct bx_truncate_options {
    const char* progname;
    bool no_create;
    bool io_blocks;
    bool size_specified;
    struct bx_truncate_size_spec size_spec;
    const char* reference_path;
    bool show_help;
    bool show_version;
    const char** paths;
    size_t path_count;
};

static void bx_truncate_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Shrink or extend the size of each FILE to the specified size.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --no-create       do not create missing files\n");
    fprintf(stream, "  -o, --io-blocks       treat SIZE as number of I/O blocks of each file\n");
    fprintf(stream, "  -r, --reference=FILE  base size on FILE\n");
    fprintf(stream, "  -s, --size=SIZE       set or adjust file size by SIZE bytes\n");
    fprintf(stream, "                         SIZE accepts leading +, -, <, >, /, or %% modifiers\n");
    fprintf(stream, "                         and suffixes K, M, ... and KB/MB/... or KiB/MiB/...\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

static bool bx_truncate_parse_size_spec(const char* text, struct bx_truncate_size_spec* spec_out) {
    if (text == NULL || text[0] == '\0' || spec_out == NULL) {
        return false;
    }

    struct bx_truncate_size_spec spec;
    spec.mode = BX_TRUNCATE_SIZE_SET;
    spec.value = 0;

    const char* p = text;
    switch (*p) {
        case '+':
            spec.mode = BX_TRUNCATE_SIZE_INCREASE;
            p++;
            break;
        case '-':
            spec.mode = BX_TRUNCATE_SIZE_DECREASE;
            p++;
            break;
        case '<':
            spec.mode = BX_TRUNCATE_SIZE_AT_MOST;
            p++;
            break;
        case '>':
            spec.mode = BX_TRUNCATE_SIZE_AT_LEAST;
            p++;
            break;
        case '/':
            spec.mode = BX_TRUNCATE_SIZE_ROUND_DOWN;
            p++;
            break;
        case '%':
            spec.mode = BX_TRUNCATE_SIZE_ROUND_UP;
            p++;
            break;
        default:
            break;
    }

    if (!bx_size_parse_scaled_uint(p, &spec.value)) {
        return false;
    }

    if ((spec.mode == BX_TRUNCATE_SIZE_ROUND_DOWN || spec.mode == BX_TRUNCATE_SIZE_ROUND_UP) && spec.value == 0) {
        return false;
    }

    *spec_out = spec;
    return true;
}

static bool bx_truncate_parse_options(int argc, char** argv, struct bx_truncate_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'c', 'c', "no-create", BX_OPTION_NO_VALUE},
        {'o', 'o', "io-blocks", BX_OPTION_NO_VALUE},
        {'r', 'r', "reference", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "size", BX_OPTION_REQUIRED_VALUE},
        {BX_TRUNCATE_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_TRUNCATE_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    struct bx_option_cursor cursor;
    size_t end = argc > 0 ? (size_t)argc : 0;
    bx_option_cursor_init(&cursor, argv, end != 0, end, BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            if (options->paths == NULL) {
                size_t capacity = end - 1;
                if (capacity > SIZE_MAX / sizeof(*options->paths) ||
                    (options->paths = malloc(capacity * sizeof(*options->paths))) == NULL) {
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
            }
            options->paths[options->path_count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE) {
                /* Preserve the established short names for --size and --reference. */
                bx_diag(diag, "option requires an argument -- '%c'", result.spec->short_name);
            }
            else if (result.short_name != 0)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        switch (result.spec->id) {
            case 'c':
                options->no_create = true;
                break;
            case 'o':
                options->io_blocks = true;
                break;
            case 'r':
                options->reference_path = result.value;
                break;
            case 's':
                if (!bx_truncate_parse_size_spec(result.value, &options->size_spec)) {
                    bx_diag(diag, "invalid size '%s'", result.value);
                    return false;
                }
                options->size_specified = true;
                break;
            case BX_TRUNCATE_OPT_HELP:
                options->show_help = true;
                break;
            case BX_TRUNCATE_OPT_VERSION:
                options->show_version = true;
                break;
            default:
                return false;
        }
    }

    if (options->show_help || options->show_version) {
        return true;
    }

    if (!options->size_specified && options->reference_path == NULL) {
        bx_diag(diag, "you must specify either --size or --reference");
        return false;
    }

    if (options->reference_path != NULL && options->size_specified && options->size_spec.mode == BX_TRUNCATE_SIZE_SET) {
        bx_diag(diag, "you must specify a relative '--size' with '--reference'");
        return false;
    }

    if (options->io_blocks && !options->size_specified) {
        bx_diag(diag, "'--io-blocks' was specified but '--size' was not");
        return false;
    }

    return true;
}

static bool bx_truncate_compute_size(const struct bx_truncate_size_spec* spec, uintmax_t base_size, uintmax_t* size_out, struct bx_diag_ctx* diag) {
    uintmax_t size = 0;

    switch (spec->mode) {
        case BX_TRUNCATE_SIZE_SET:
            size = spec->value;
            break;
        case BX_TRUNCATE_SIZE_INCREASE:
            if (base_size > UINTMAX_MAX - spec->value) {
                bx_diag(diag, "size overflow");
                return false;
            }
            size = base_size + spec->value;
            break;
        case BX_TRUNCATE_SIZE_DECREASE:
            size = (base_size > spec->value) ? (base_size - spec->value) : 0;
            break;
        case BX_TRUNCATE_SIZE_AT_MOST:
            size = (base_size > spec->value) ? spec->value : base_size;
            break;
        case BX_TRUNCATE_SIZE_AT_LEAST:
            size = (base_size < spec->value) ? spec->value : base_size;
            break;
        case BX_TRUNCATE_SIZE_ROUND_DOWN:
            if (spec->value == 0) {
                bx_diag(diag, "division by zero");
                return false;
            }
            size = (base_size / spec->value) * spec->value;
            break;
        case BX_TRUNCATE_SIZE_ROUND_UP:
            if (spec->value == 0) {
                bx_diag(diag, "division by zero");
                return false;
            }
            if (base_size % spec->value == 0) {
                size = base_size;
            }
            else {
                uintmax_t delta = spec->value - (base_size % spec->value);
                if (base_size > UINTMAX_MAX - delta) {
                    bx_diag(diag, "size overflow");
                    return false;
                }
                size = base_size + delta;
            }
            break;
        default:
            bx_diag(diag, "invalid size mode");
            return false;
    }

    *size_out = size;
    return true;
}

static bool bx_truncate_uintmax_to_off_t(uintmax_t value, off_t* out) {
    off_t converted = (off_t)value;
    if ((uintmax_t)converted != value) {
        return false;
    }
    if ((off_t)-1 < (off_t)0 && converted < 0) {
        return false;
    }
    *out = converted;
    return true;
}

static bool
bx_truncate_resolve_target_size(const struct bx_truncate_options* options, int fd, uintmax_t reference_size, bool has_reference, const char* path, off_t* target_out, struct bx_diag_ctx* diag) {
    uintmax_t target_size = 0;
    struct bx_truncate_size_spec effective_spec = options->size_spec;
    struct stat st;
    bool have_stat = false;

    bool need_stat = false;
    if (options->io_blocks && options->size_specified) {
        need_stat = true;
    }
    if (options->size_specified && !has_reference && options->size_spec.mode != BX_TRUNCATE_SIZE_SET) {
        need_stat = true;
    }

    if (need_stat) {
        if (fstat(fd, &st) != 0) {
            bx_perror_path(diag, path);
            return false;
        }
        have_stat = true;
    }

    if (options->io_blocks && options->size_specified) {
        if (st.st_blksize <= 0) {
            bx_diag(diag, "%s: invalid I/O block size", path);
            return false;
        }

        uintmax_t block_size = (uintmax_t)st.st_blksize;
        if (effective_spec.value > UINTMAX_MAX / block_size) {
            bx_diag(diag, "%s: size overflow", path);
            return false;
        }
        effective_spec.value *= block_size;
    }

    if (!options->size_specified) {
        target_size = reference_size;
    }
    else if (has_reference) {
        if (!bx_truncate_compute_size(&effective_spec, reference_size, &target_size, diag)) {
            return false;
        }
    }
    else if (effective_spec.mode == BX_TRUNCATE_SIZE_SET) {
        target_size = effective_spec.value;
    }
    else {
        if (!have_stat) {
            if (fstat(fd, &st) != 0) {
                bx_perror_path(diag, path);
                return false;
            }
            have_stat = true;
        }
        if (!bx_truncate_compute_size(&effective_spec, (uintmax_t)st.st_size, &target_size, diag)) {
            return false;
        }
    }

    if (!bx_truncate_uintmax_to_off_t(target_size, target_out)) {
        bx_diag(diag, "%s: size too large", path);
        return false;
    }

    return true;
}

static bool bx_truncate_get_reference_size(const char* path, uintmax_t* size_out, struct bx_diag_ctx* diag) {
    struct stat st;
    if (stat(path, &st) != 0) {
        bx_perror_path(diag, path);
        return false;
    }
    *size_out = (uintmax_t)st.st_size;
    return true;
}

static bool bx_truncate_apply_path(const char* path, const struct bx_truncate_options* options, uintmax_t reference_size, bool has_reference, struct bx_diag_ctx* diag) {
    int flags = O_WRONLY;
    if (!options->no_create) {
        flags |= O_CREAT;
    }

    int fd = bx_fd_open_cloexec(path, flags, 0666u);
    if (fd < 0) {
        if (options->no_create && errno == ENOENT) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }

    off_t target_size = 0;
    if (!bx_truncate_resolve_target_size(options, fd, reference_size, has_reference, path, &target_size, diag)) {
        bx_fd_cleanup(&fd);
        return false;
    }

    if (bx_fd_ftruncate(fd, target_size) != 0) {
        bx_perror_path(diag, path);
        bx_fd_cleanup(&fd);
        return false;
    }

    if (!bx_fd_close(&fd, path, diag)) {
        return false;
    }

    return true;
}

static int bx_truncate_run(const struct bx_truncate_options* options, struct bx_diag_ctx* diag) {
    if (options->show_help) {
        bx_truncate_print_help(stdout, options->progname);
        return 0;
    }
    if (options->show_version) {
        bx_cli_print_version(options->progname);
        return 0;
    }
    if (options->path_count == 0) {
        bx_diag(diag, "missing operand");
        return diag->exit_status;
    }

    uintmax_t reference_size = 0;
    bool has_reference = options->reference_path != NULL;
    if (has_reference && !bx_truncate_get_reference_size(options->reference_path, &reference_size, diag))
        return diag->exit_status;

    /* An absolute byte count can be rejected without opening any target. */
    if (options->size_specified && !options->io_blocks && options->size_spec.mode == BX_TRUNCATE_SIZE_SET) {
        off_t size;
        if (!bx_truncate_uintmax_to_off_t(options->size_spec.value, &size)) {
            bx_diag(diag, "%s: size too large", options->paths[0]);
            return diag->exit_status;
        }
    }

    for (size_t i = 0; i < options->path_count; i++) {
        (void)bx_truncate_apply_path(options->paths[i], options, reference_size, has_reference, diag);
    }
    return diag->exit_status;
}

int bx_truncate_main(int argc, char** argv) {
    struct bx_truncate_options options = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "truncate"),
    };
    struct bx_diag_ctx diag = {.progname = options.progname};
    int status;
    if (bx_truncate_parse_options(argc, argv, &options, &diag))
        status = bx_truncate_run(&options, &diag);
    else
        status = diag.exit_status != 0 ? diag.exit_status : 1;
    free(options.paths);
    return status;
}
