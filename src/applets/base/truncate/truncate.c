#include <fcntl.h>
#include <getopt.h>
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

struct bx_truncate_options {
    const char* progname;
    bool no_create;
    bool io_blocks;
    bool size_specified;
    struct bx_truncate_size_spec size_spec;
    const char* reference_path;
    bool show_help;
    bool show_version;
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

static bool bx_truncate_parse_size_magnitude(const char* text, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    size_t pos = 0;
    while (text[pos] >= '0' && text[pos] <= '9') {
        pos++;
    }
    if (pos == 0) {
        return false;
    }

    char digits[64];
    if (pos >= sizeof(digits)) {
        return false;
    }
    memcpy(digits, text, pos);
    digits[pos] = '\0';

    uintmax_t value = 0;
    if (!bx_size_parse_uint(digits, &value)) {
        return false;
    }

    uintmax_t multiplier = 0;
    if (!bx_size_suffix_multiplier(text + pos, &multiplier)) {
        return false;
    }

    if (value != 0 && multiplier > UINTMAX_MAX / value) {
        return false;
    }

    *value_out = value * multiplier;
    return true;
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

    if (!bx_truncate_parse_size_magnitude(p, &spec.value)) {
        return false;
    }

    if ((spec.mode == BX_TRUNCATE_SIZE_ROUND_DOWN || spec.mode == BX_TRUNCATE_SIZE_ROUND_UP) && spec.value == 0) {
        return false;
    }

    *spec_out = spec;
    return true;
}

static bool bx_truncate_parse_options(int argc, char** argv, struct bx_truncate_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"no-create", no_argument, NULL, 'c'},
        {"io-blocks", no_argument, NULL, 'o'},
        {"reference", required_argument, NULL, 'r'},
        {"size", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "truncate");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:cor:s:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'c':
                options->no_create = true;
                break;
            case 'o':
                options->io_blocks = true;
                break;
            case 'r':
                options->reference_path = optarg;
                break;
            case 's':
                if (!bx_truncate_parse_size_spec(optarg, &options->size_spec)) {
                    bx_diag(diag, "invalid size '%s'", (optarg != NULL) ? optarg : "");
                    return false;
                }
                options->size_specified = true;
                break;
            case 1:
                options->show_help = true;
                break;
            case 2:
                options->show_version = true;
                break;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;

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

static void bx_truncate_close_quietly(int fd) {
    if (fd >= 0) {
        (void)close(fd);
    }
}

static bool bx_truncate_apply_path(const char* path, const struct bx_truncate_options* options, uintmax_t reference_size, bool has_reference, struct bx_diag_ctx* diag) {
    int flags = O_WRONLY;
    if (!options->no_create) {
        flags |= O_CREAT;
    }

    int fd = open(path, flags, 0666u);
    if (fd < 0) {
        if (options->no_create && errno == ENOENT) {
            return true;
        }
        bx_perror_path(diag, path);
        return false;
    }

    off_t target_size = 0;
    if (!bx_truncate_resolve_target_size(options, fd, reference_size, has_reference, path, &target_size, diag)) {
        bx_truncate_close_quietly(fd);
        return false;
    }

    if (ftruncate(fd, target_size) != 0) {
        bx_perror_path(diag, path);
        bx_truncate_close_quietly(fd);
        return false;
    }

    if (close(fd) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    return true;
}

int bx_truncate_main(int argc, char** argv) {
    struct bx_truncate_options options;
    struct bx_diag_ctx diag = {
        .progname = "truncate",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_truncate_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_truncate_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    uintmax_t reference_size = 0;
    bool has_reference = options.reference_path != NULL;
    if (has_reference && !bx_truncate_get_reference_size(options.reference_path, &reference_size, &diag)) {
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        (void)bx_truncate_apply_path(argv[i], &options, reference_size, has_reference, &diag);
    }

    return diag.exit_status;
}
