/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "applets/archive/archive_codec.h"
#include "bx/diag.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/xreadwrite.h"

static void input_error(void* user, const char* progname, const char* message) {
    const char* name = *(const char**)user;
    fprintf(stderr, "%s: %s: %s\n", progname, name, message);
}

static int decode(const char* name, const struct bx_archive_codec* codec,
                  struct bx_diag_ctx* diag, bool* output_error) {
    const struct bx_archive_codec_input_options options = {
        .archive_path = name,
        .required_codec = codec,
        .seek_mode = BX_ARCHIVE_CODEC_SEEK_DISABLE,
        .xz_memory_limit = UINT64_C(256) << 20,
    };
    struct bx_archive_codec_input* input;
    if (!bx_archive_codec_input_open(&input, &options, diag))
        return 1;

    const char* label = strcmp(name, "-") == 0 ? "(stdin)" : name;
    struct bx_diag_ctx reader_diag = {
        .progname = diag->progname, .emit = input_error,
        .emit_user = &label,
    };
    int status = 0;
    unsigned char buffer[16384];
    for (;;) {
        size_t len;
        if (!bx_archive_codec_input_read_some(input, buffer, sizeof(buffer), &len, &reader_diag) ||
            (!len && !bx_archive_codec_input_finish_success(input, &reader_diag))) {
            status = codec == bx_archive_codec_bzip2() && bx_archive_codec_input_decode_failed(input) ? 2 : 1;
            break;
        }
        if (!len)
            break;
        if (!bx_xwrite_all(STDOUT_FILENO, buffer, len)) {
            bx_perror_path(diag, "standard output");
            *output_error = true;
            status = 1;
            break;
        }
    }
    bx_archive_codec_input_close(input);
    return status;
}

static int archive_cat(int argc, char** argv, const char* progname,
                       const struct bx_archive_codec* codec) {
    struct bx_diag_ctx diag = {.progname = progname};
    static const struct option options[] = {
        {"decompress", no_argument, NULL, 'd'},
        {"stdout", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 'h'},
        {0},
    };
    int opt, operand = 1;
    bx_args_getopt_reset();
    while ((opt = bx_args_getopt_long_interspersed(argc, argv, "-:cdh", options, &operand)) != -1) {
        switch (opt) {
        case 'c':
        case 'd':
            break;
        case 'h':
            printf("Usage: %s [-cd] [--] [FILE...]\n"
                   "Decompress %s data to standard output without changing the input files.\n"
                   "With no files or a '-' operand, read standard input.\n",
                   progname, bx_archive_codec_name(codec));
            return 0;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    bool output_error = false;
    if (optind == argc)
        return decode("-", codec, &diag, &output_error);
    int status = 0;
    for (int i = optind; i < argc && !output_error; i++) {
        int result = decode(argv[i], codec, &diag, &output_error);
        if (status < result)
            status = result;
    }
    return status;
}

int bx_bzcat_main(int argc, char** argv) {
    return archive_cat(argc, argv, "bzcat", bx_archive_codec_bzip2());
}

int bx_xzcat_main(int argc, char** argv) {
    return archive_cat(argc, argv, "xzcat", bx_archive_codec_xz());
}
