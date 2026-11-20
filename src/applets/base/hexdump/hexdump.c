/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "crypto/digest_util.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
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
    static const struct option options[] = {{"help", no_argument, NULL, 'h'}, {0}};
    struct bx_diag_ctx diag = {.progname = "hexdump"};
    struct dump dump = {0};
    bool mode_selected = false;
    int opt, operand = 1;
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-:Ce:hv", options, &operand)) != -1) {
        switch (opt) {
        case 'C':
        case 'e':
            if (mode_selected || (opt == 'e' && strcmp(optarg, "1/4 \"%u\"") != 0)) {
                bx_diag(&diag, "choose exactly one of -C or -e '1/4 \"%%u\"'");
                return 1;
            }
            dump.canonical = opt == 'C';
            mode_selected = true;
            break;
        case 'v':
            dump.verbose = true;
            break;
        case 'h':
            usage(stdout);
            return 0;
        case ':':
            bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            return 1;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (!mode_selected) {
        usage(stderr);
        return 1;
    }
    size_t width = dump.canonical ? 16 : sizeof(uint32_t);
    if (optind == argc && !consume(&dump, width, "-", &diag))
        return 1;
    for (int i = optind; i < argc; i++)
        if (!consume(&dump, width, argv[i], &diag))
            return 1;
    if ((dump.used && !emit(&dump, width)) ||
        (dump.canonical && dump.offset && printf("%08" PRIxMAX "\n", dump.offset) < 0) ||
        fflush(stdout) == EOF) {
        bx_perror_path(&diag, "standard output");
        return 1;
    }
    return 0;
}
