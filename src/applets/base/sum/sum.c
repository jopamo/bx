#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"
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
    static const struct bx_option_spec specs[] = {
        {'r', 'r', NULL, BX_OPTION_NO_VALUE},
        {'s', 's', "sysv", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'v', 'v', "version", BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sum"),
        .exit_status = 1,
    };

    bool sysv = false;
    const char** files = NULL;
    size_t file_count = 0;
    int status = 1;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!files) {
                files = calloc((size_t)argc - option.index, sizeof(*files));
                if (!files) {
                    bx_perror_path(&diag, "allocate input operands");
                    goto done;
                }
            }
            files[file_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
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
                status = 0;
                goto done;
            case 'v':
                printf("sum (bx) %s\n", BX_VERSION);
                status = 0;
                goto done;
        }
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (file_count == 0) {
        bool ok = sysv ? sum_sysv(stdin, NULL, false, &writer) : sum_bsd(stdin, NULL, false, &writer);
        return ok && bx_line_writer_flush(&writer) ? 0 : 1;
    }

    for (size_t i = 0; i < file_count; i++) {
        const char* path = files[i];
        FILE* f = fopen(path, "r");
        if (!f) {
            bx_perror_path(&diag, path);
            continue;
        }
        bool ok = sysv ? sum_sysv(f, path, true, &writer) : sum_bsd(f, path, true, &writer);
        fclose(f);
        if (!ok)
            goto done;
    }

    status = bx_line_writer_flush(&writer) ? 0 : 1;
done:
    free(files);
    return status;
}
