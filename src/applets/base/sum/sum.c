#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/size_parse.h"

static bool sum_write_bsd_result(struct bx_line_writer* writer, uint16_t checksum, uintmax_t blocks, const char* name, bool print_name) {
    char buffer[128];
    int len = snprintf(buffer, sizeof(buffer), "%05u %5" PRIuMAX, checksum, blocks);
    return len >= 0 && (size_t)len < sizeof(buffer)
        && bx_line_writer_write(writer, buffer, (size_t)len)
        && (!print_name || bx_line_writer_putc(writer, ' '))
        && (name == NULL || bx_line_writer_puts(writer, name))
        && bx_line_writer_putc(writer, '\n');
}

static bool sum_write_sysv_result(struct bx_line_writer* writer, uint16_t checksum, uintmax_t blocks, const char* name, bool print_name) {
    char buffer[128];
    int len = snprintf(buffer, sizeof(buffer), "%u %" PRIuMAX, checksum, blocks);
    return len >= 0 && (size_t)len < sizeof(buffer)
        && bx_line_writer_write(writer, buffer, (size_t)len)
        && (!print_name || bx_line_writer_putc(writer, ' '))
        && (name == NULL || bx_line_writer_puts(writer, name))
        && bx_line_writer_putc(writer, '\n');
}

static bool sum_bsd(FILE* f, const char* name, bool print_name, struct bx_line_writer* writer) {
    uint16_t checksum = 0;
    uintmax_t total_bytes = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        checksum = (checksum >> 1) + ((checksum & 1) << 15);
        checksum += (uint8_t)c;
        total_bytes++;
    }
    uintmax_t blocks = 0;
    (void)bx_size_block_count_ceil(total_bytes, 1024u, &blocks);
    return sum_write_bsd_result(writer, checksum, blocks, name, print_name);
}

static bool sum_sysv(FILE* f, const char* name, bool print_name, struct bx_line_writer* writer) {
    uint32_t s = 0;
    uintmax_t total_bytes = 0;
    int c;
    while ((c = getc(f)) != EOF) {
        s += (uint8_t)c;
        total_bytes++;
    }
    uint32_t r = (s & 0xffff) + (s >> 16);
    uint16_t checksum = (r & 0xffff) + (r >> 16);
    uintmax_t blocks = 0;
    (void)bx_size_block_count_ceil(total_bytes, 512u, &blocks);
    return sum_write_sysv_result(writer, checksum, blocks, name, print_name);
}

int bx_sum_main(int argc, char** argv) {
    static const struct option long_options[] = {{"sysv", no_argument, NULL, 's'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sum"),
        .exit_status = 1,
    };

    bool sysv = false;
    int c;
    bx_args_getopt_reset();
    while ((c = bx_args_getopt_long(argc, argv, ":rshv", long_options, NULL)) != -1) {
        switch (c) {
            case 'r':
                sysv = false;
                break;
            case 's':
                sysv = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Print checksum and block counts for each FILE.\n");
                printf("\n");
                printf("  -r              use BSD sum algorithm, 1K blocks (default)\n");
                printf("  -s, --sysv      use System V sum algorithm, 512B blocks\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("sum (bx) %s\n", BX_VERSION);
                return 0;
            default:
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                return 1;
        }
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (optind == argc) {
        bool ok = sysv ? sum_sysv(stdin, NULL, false, &writer) : sum_bsd(stdin, NULL, false, &writer);
        return ok && bx_line_writer_flush(&writer) ? 0 : 1;
    }

    for (int i = optind; i < argc; i++) {
        FILE* f = fopen(argv[i], "r");
        if (!f) {
            bx_perror_path(&diag, argv[i]);
            continue;
        }
        bool ok = sysv ? sum_sysv(f, argv[i], true, &writer) : sum_bsd(f, argv[i], true, &writer);
        fclose(f);
        if (!ok)
            return 1;
    }

    return bx_line_writer_flush(&writer) ? 0 : 1;
}
