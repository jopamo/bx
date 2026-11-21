#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"

enum bx_free_unit {
    BX_FREE_UNIT_KIB,
    BX_FREE_UNIT_BYTES,
    BX_FREE_UNIT_MIB,
    BX_FREE_UNIT_GIB,
    BX_FREE_UNIT_HUMAN,
};

struct bx_free_options {
    const char* progname;
    enum bx_free_unit unit;
    bool show_total;
    bool show_help;
    bool show_version;
};

struct bx_free_values {
    uintmax_t mem_total;
    uintmax_t mem_free;
    uintmax_t mem_available;
    uintmax_t buffers;
    uintmax_t cached;
    uintmax_t shmem;
    uintmax_t sreclaimable;
    uintmax_t swap_total;
    uintmax_t swap_free;
    bool have_mem_total;
    bool have_mem_free;
    bool have_mem_available;
    bool have_buffers;
    bool have_cached;
    bool have_shmem;
    bool have_swap_total;
    bool have_swap_free;
};

static uintmax_t bx_free_add_sat(uintmax_t left, uintmax_t right) {
    return left > UINTMAX_MAX - right ? UINTMAX_MAX : left + right;
}

static uintmax_t bx_free_mul_sat(uintmax_t value, uintmax_t factor) {
    return value != 0u && factor > UINTMAX_MAX / value ? UINTMAX_MAX : value * factor;
}

static void bx_free_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Display amount of free and used memory.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --bytes         show values in bytes\n");
    fprintf(stream, "  -k, --kibi          show values in KiB (default)\n");
    fprintf(stream, "  -m, --mebi          show values in MiB\n");
    fprintf(stream, "  -g, --gibi          show values in GiB\n");
    fprintf(stream, "  -h, --human         show human-readable values\n");
    fprintf(stream, "  -t, --total         show a total line\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

static bool bx_free_parse_options(int argc, char** argv, struct bx_free_options* options,
                                  struct bx_diag_ctx* diag) {
    enum {
        OPT_HELP = 1000,
        OPT_VERSION,
    };
    static const struct option long_options[] = {
        {"bytes", no_argument, NULL, 'b'},
        {"kibi", no_argument, NULL, 'k'},
        {"mebi", no_argument, NULL, 'm'},
        {"gibi", no_argument, NULL, 'g'},
        {"human", no_argument, NULL, 'h'},
        {"total", no_argument, NULL, 't'},
        {"help", no_argument, NULL, OPT_HELP},
        {"version", no_argument, NULL, OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "free");
    options->unit = BX_FREE_UNIT_KIB;
    diag->progname = options->progname;

    bx_args_getopt_reset();
    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+bkmght", long_options, NULL);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'b':
                options->unit = BX_FREE_UNIT_BYTES;
                break;
            case 'k':
                options->unit = BX_FREE_UNIT_KIB;
                break;
            case 'm':
                options->unit = BX_FREE_UNIT_MIB;
                break;
            case 'g':
                options->unit = BX_FREE_UNIT_GIB;
                break;
            case 'h':
                options->unit = BX_FREE_UNIT_HUMAN;
                break;
            case 't':
                options->show_total = true;
                break;
            case OPT_HELP:
                options->show_help = true;
                return true;
            case OPT_VERSION:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (optind < argc) {
        bx_diag(diag, "unexpected operand '%s'", argv[optind]);
        return false;
    }
    return true;
}

static bool bx_free_key_is(const char* key, const char* expected) {
    return strcmp(key, expected) == 0;
}

static bool bx_free_parse_line(char* line, struct bx_free_values* values) {
    char* colon = strchr(line, ':');
    if (colon == NULL) {
        return true;
    }

    *colon = '\0';
    char* key_end = line + strlen(line);
    while (key_end > line && isspace((unsigned char)key_end[-1])) {
        key_end--;
    }
    *key_end = '\0';

    char* value = colon + 1;
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }
    char* value_end = value;
    while (*value_end != '\0' && !isspace((unsigned char)*value_end)) {
        value_end++;
    }
    *value_end = '\0';
    if (*value == '\0') {
        return true;
    }

    uintmax_t number;
    if (!bx_size_parse_uint(value, &number)) {
        return false;
    }

    if (bx_free_key_is(line, "MemTotal")) {
        values->mem_total = number;
        values->have_mem_total = true;
    }
    else if (bx_free_key_is(line, "MemFree")) {
        values->mem_free = number;
        values->have_mem_free = true;
    }
    else if (bx_free_key_is(line, "MemAvailable")) {
        values->mem_available = number;
        values->have_mem_available = true;
    }
    else if (bx_free_key_is(line, "Buffers")) {
        values->buffers = number;
        values->have_buffers = true;
    }
    else if (bx_free_key_is(line, "Cached")) {
        values->cached = number;
        values->have_cached = true;
    }
    else if (bx_free_key_is(line, "Shmem")) {
        values->shmem = number;
        values->have_shmem = true;
    }
    else if (bx_free_key_is(line, "SReclaimable")) {
        values->sreclaimable = number;
    }
    else if (bx_free_key_is(line, "SwapTotal")) {
        values->swap_total = number;
        values->have_swap_total = true;
    }
    else if (bx_free_key_is(line, "SwapFree")) {
        values->swap_free = number;
        values->have_swap_free = true;
    }

    return true;
}

static bool bx_free_read_meminfo(struct bx_free_values* values, struct bx_diag_ctx* diag) {
    FILE* stream = fopen("/proc/meminfo", "r");
    if (stream == NULL) {
        bx_diag(diag, "cannot read /proc/meminfo: %s", strerror(errno));
        return false;
    }

    memset(values, 0, sizeof(*values));
    char* line = NULL;
    size_t line_capacity = 0;
    bool ok = true;
    while (getline(&line, &line_capacity, stream) >= 0) {
        if (!bx_free_parse_line(line, values)) {
            bx_diag(diag, "invalid numeric value in /proc/meminfo");
            ok = false;
            break;
        }
    }
    if (ok && ferror(stream)) {
        bx_diag(diag, "cannot read /proc/meminfo: %s", strerror(errno));
        ok = false;
    }

    free(line);
    fclose(stream);

    if (!ok) {
        return false;
    }
    if (!values->have_mem_total || !values->have_mem_free ||
        !values->have_buffers || !values->have_cached ||
        !values->have_swap_total || !values->have_swap_free) {
        bx_diag(diag, "/proc/meminfo is missing required memory fields");
        return false;
    }
    return true;
}

static void bx_free_format_value(uintmax_t kib, enum bx_free_unit unit, char* buffer,
                                 size_t buffer_size) {
    uintmax_t value = kib;

    switch (unit) {
        case BX_FREE_UNIT_KIB:
            snprintf(buffer, buffer_size, "%" PRIuMAX, value);
            return;
        case BX_FREE_UNIT_BYTES:
            snprintf(buffer, buffer_size, "%" PRIuMAX, bx_free_mul_sat(value, 1024u));
            return;
        case BX_FREE_UNIT_MIB:
            snprintf(buffer, buffer_size, "%" PRIuMAX, value / 1024u);
            return;
        case BX_FREE_UNIT_GIB:
            snprintf(buffer, buffer_size, "%" PRIuMAX, value / (1024u * 1024u));
            return;
        case BX_FREE_UNIT_HUMAN:
            bx_size_format_human_round(bx_free_mul_sat(value, 1024u), 1024u, "BKMGTPE",
                                       true, buffer, buffer_size);
            return;
    }
}

static void bx_free_print_header(void) {
    printf("%-6s %11s %11s %11s %11s %11s %11s\n", "", "total", "used", "free",
           "shared", "buff/cache", "available");
}

static void bx_free_print_row(const char* label, const char* const cells[6], size_t cell_count) {
    printf("%-6s", label);
    for (size_t i = 0; i < cell_count; i++) {
        printf(" %11s", cells[i]);
    }
    putchar('\n');
}

static void bx_free_make_cells(const uintmax_t values[6], enum bx_free_unit unit,
                               char cells[6][64]) {
    for (size_t i = 0; i < 6; i++) {
        bx_free_format_value(values[i], unit, cells[i], sizeof(cells[i]));
    }
}

int bx_free_main(int argc, char** argv) {
    struct bx_free_options options;
    struct bx_diag_ctx diag = {
        .progname = "free",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_free_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }
    if (options.show_help) {
        bx_free_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    struct bx_free_values values;
    if (!bx_free_read_meminfo(&values, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    uintmax_t available = values.mem_available;
    if (!values.have_mem_available) {
        available = bx_free_add_sat(values.mem_free, values.buffers);
        available = bx_free_add_sat(available, values.cached);
        available = bx_free_add_sat(available, values.sreclaimable);
    }
    if (available > values.mem_total) {
        available = values.mem_total;
    }
    uintmax_t used = values.mem_total - available;
    uintmax_t buff_cache = bx_free_add_sat(values.buffers, values.cached);
    buff_cache = bx_free_add_sat(buff_cache, values.sreclaimable);
    uintmax_t swap_used = values.swap_total >= values.swap_free
                              ? values.swap_total - values.swap_free
                              : 0u;

    const uintmax_t mem_values[6] = {
        values.mem_total,
        used,
        values.mem_free,
        values.have_shmem ? values.shmem : 0u,
        buff_cache,
        available,
    };
    const uintmax_t swap_values[6] = {
        values.swap_total,
        swap_used,
        values.swap_free,
    };

    char mem_cells[6][64];
    char swap_cells[6][64];
    char total_cells[6][64];
    bx_free_make_cells(mem_values, options.unit, mem_cells);
    bx_free_make_cells(swap_values, options.unit, swap_cells);

    bx_free_print_header();
    const char* const mem_row[6] = {
        mem_cells[0], mem_cells[1], mem_cells[2], mem_cells[3], mem_cells[4], mem_cells[5],
    };
    const char* const swap_row[6] = {
        swap_cells[0], swap_cells[1], swap_cells[2], NULL, NULL, NULL,
    };
    bx_free_print_row("Mem:", mem_row, 6);
    bx_free_print_row("Swap:", swap_row, 3);

    if (options.show_total) {
        const uintmax_t total_values[3] = {
            bx_free_add_sat(values.mem_total, values.swap_total),
            bx_free_add_sat(used, swap_used),
            bx_free_add_sat(values.mem_free, values.swap_free),
        };
        for (size_t i = 0; i < 3; i++) {
            bx_free_format_value(total_values[i], options.unit, total_cells[i],
                                 sizeof(total_cells[i]));
        }
        const char* const total_row[6] = {
            total_cells[0], total_cells[1], total_cells[2], NULL, NULL, NULL,
        };
        bx_free_print_row("Total:", total_row, 3);
    }

    if (fflush(stdout) != 0 || ferror(stdout)) {
        bx_diag(&diag, "write error: %s", strerror(errno ? errno : EIO));
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }
    return 0;
}
