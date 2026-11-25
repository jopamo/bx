/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "crypto/digest_util.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"
#include "lib/xreadwrite.h"

struct dump {
    uintmax_t offset;
    unsigned char pending[16];
    unsigned char previous[16];
    size_t used;
    bool canonical;
    bool verbose;
    bool repeating;
};

static bool canonical_line(uintmax_t offset, const unsigned char* bytes, size_t len) {
    char hex[2 * 16 + 1], columns[50], printable[17];
    bx_hex_encode_lower(bytes, len, hex);
    size_t p = 0;
    for (size_t i = 0; i < 16; i++) {
        columns[p++] = i < len ? hex[2 * i] : ' ';
        columns[p++] = i < len ? hex[2 * i + 1] : ' ';
        columns[p++] = ' ';
        if (i == 7)
            columns[p++] = ' ';
    }
    columns[p] = '\0';
    for (size_t i = 0; i < len; i++)
        printable[i] = bytes[i] >= 0x20 && bytes[i] <= 0x7e ? (char)bytes[i] : '.';
    printable[len] = '\0';
    return printf("%08" PRIxMAX "  %s |%s|\n", offset, columns, printable) >= 0;
}

static bool emit(struct dump* dump, size_t width) {
    if (UINTMAX_MAX - dump->offset < dump->used) {
        errno = EOVERFLOW;
        return false;
    }
    bool ok = true;
    if (!dump->verbose && dump->used == width && dump->offset &&
        memcmp(dump->pending, dump->previous, width) == 0) {
        if (!dump->repeating)
            ok = fputs("*\n", stdout) != EOF;
        dump->repeating = true;
    } else {
        if (dump->canonical)
            ok = canonical_line(dump->offset, dump->pending, dump->used);
        else {
            uint32_t word;
            memset(dump->pending + dump->used, 0, width - dump->used);
            memcpy(&word, dump->pending, sizeof(word));
            ok = printf("%" PRIu32, word) >= 0;
        }
        dump->repeating = false;
    }
    memcpy(dump->previous, dump->pending, width);
    dump->offset += dump->used;
    dump->used = 0;
    return ok;
}

static bool consume(struct dump* dump, size_t width, const char* name,
                    struct bx_diag_ctx* diag) {
    bool standard = strcmp(name, "-") == 0;
    int fd = standard ? STDIN_FILENO : bx_fd_open_read(name, diag);
    if (fd < 0)
        return false;
    unsigned char block[8192];
    bool ok = true;
    for (;;) {
        ssize_t n = bx_xread(fd, block, sizeof(block));
        if (n <= 0) {
            if (n < 0) {
                bx_perror_path(diag, name);
                ok = false;
            }
            break;
        }
        for (size_t p = 0; p < (size_t)n;) {
            size_t count = width - dump->used;
            if (count > (size_t)n - p)
                count = (size_t)n - p;
            memcpy(dump->pending + dump->used, block + p, count);
            dump->used += count;
            p += count;
            if (dump->used == width && !emit(dump, width)) {
                bx_perror_path(diag, "standard output");
                ok = false;
                goto done;
            }
        }
    }
done:
    if (!standard && !bx_fd_close(&fd, name, diag))
        ok = false;
    return ok;
}

static void usage(FILE* stream) {
    fputs("Usage: hexdump [-v] (-C | -e '1/4 \"%u\"') [--] [FILE...]\n"
          "Print canonical hex and ASCII (-C), or unsigned\n"
          "32-bit words in host byte order using the shown -e format.\n"
          "-v prints all blocks instead of suppressing repeated blocks.\n", stream);
}

int bx_hexdump_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'C', 'C', NULL, BX_OPTION_NO_VALUE},
        {'e', 'e', NULL, BX_OPTION_REQUIRED_VALUE},
        {'v', 'v', NULL, BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {.progname = "hexdump"};
    struct dump dump = {0};
    bool mode_selected = false;
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
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'C':
            case 'e':
                if (mode_selected || (option.spec->id == 'e' && strcmp(option.value, "1/4 \"%u\"") != 0)) {
                    bx_diag(&diag, "choose exactly one of -C or -e '1/4 \"%%u\"'");
                    goto done;
                }
                dump.canonical = option.spec->id == 'C';
                mode_selected = true;
                break;
            case 'v':
                dump.verbose = true;
                break;
            case 'h':
                usage(stdout);
                status = 0;
                goto done;
        }
    }
    if (!mode_selected) {
        usage(stderr);
        goto done;
    }
    size_t width = dump.canonical ? 16 : sizeof(uint32_t);
    if (!file_count && !consume(&dump, width, "-", &diag))
        goto done;
    for (size_t i = 0; i < file_count; i++)
        if (!consume(&dump, width, files[i], &diag))
            goto done;
    if ((dump.used && !emit(&dump, width)) ||
        (dump.canonical && dump.offset && printf("%08" PRIxMAX "\n", dump.offset) < 0) ||
        fflush(stdout) == EOF) {
        bx_perror_path(&diag, "standard output");
        goto done;
    }
    status = 0;
done:
    free(files);
    return status;
}
