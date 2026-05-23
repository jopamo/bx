#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"

static void next_suffix(char* suffix, int length, bool numeric, bool hex) {
    for (int i = length - 1; i >= 0; i--) {
        if (numeric) {
            if (suffix[i] < '9') {
                suffix[i]++;
                return;
            }
            suffix[i] = '0';
        }
        else if (hex) {
            if (suffix[i] < '9') {
                suffix[i]++;
                return;
            }
            if (suffix[i] == '9') {
                suffix[i] = 'a';
                return;
            }
            if (suffix[i] < 'f') {
                suffix[i]++;
                return;
            }
            suffix[i] = '0';
        }
        else {
            if (suffix[i] < 'z') {
                suffix[i]++;
                return;
            }
            suffix[i] = 'a';
        }
    }
}

static void bx_split_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE [PREFIX]]\n", progname);
    fprintf(stream, "Output pieces of FILE to PREFIXaa, PREFIXab, ...; default size is 1000 lines.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --suffix-length=N      generate suffixes of length N (default 2)\n");
    fprintf(stream, "  -b, --bytes=SIZE           put SIZE bytes per output file\n");
    fprintf(stream, "  -d, --numeric-suffixes[=FROM]  use numeric suffixes instead of alphabetic\n");
    fprintf(stream, "  -l, --lines=NUMBER         put NUMBER lines per output file\n");
    fprintf(stream, "  -x, --hex-suffixes[=FROM]  use hexadecimal suffixes instead of alphabetic\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

int bx_split_main(int argc, char** argv) {
    static const struct option long_options[] = {{"bytes", required_argument, NULL, 'b'},
                                                 {"lines", required_argument, NULL, 'l'},
                                                 {"suffix-length", required_argument, NULL, 'a'},
                                                 {"numeric-suffixes", optional_argument, NULL, 'd'},
                                                 {"hex-suffixes", optional_argument, NULL, 'x'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'v'},
                                                 {NULL, 0, NULL, 0}};

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "split");
    struct bx_diag_ctx diag = {.progname = progname};
    long long split_size = 0;
    bool split_by_lines = true;
    long long lines_per_file = 1000;
    int suffix_length = 2;
    bool numeric = false;
    bool hex = false;

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "b:l:a:dx", long_options, NULL)) != -1) {
        switch (c) {
            case 'b': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_block_size(optarg, &parsed)
                    || parsed > (uintmax_t)LLONG_MAX) {
                    bx_diag(&diag, "invalid number of bytes: '%s'", optarg);
                    return 1;
                }
                split_size = (long long)parsed;
                split_by_lines = false;
                break;
            }
            case 'l': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_uint(optarg, &parsed)
                    || parsed == 0u
                    || parsed > (uintmax_t)LLONG_MAX) {
                    bx_diag(&diag, "invalid number of lines: '%s'", optarg);
                    return 1;
                }
                lines_per_file = (long long)parsed;
                split_by_lines = true;
                break;
            }
            case 'a': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_uint(optarg, &parsed)
                    || parsed == 0u
                    || parsed > (uintmax_t)INT32_MAX) {
                    bx_diag(&diag, "invalid suffix length: '%s'", optarg);
                    return 1;
                }
                suffix_length = (int)parsed;
                break;
            }
            case 'd':
                numeric = true;
                hex = false;
                break;
            case 'x':
                hex = true;
                numeric = false;
                break;
            case 'h':
                bx_split_print_help(stdout, progname);
                return 0;
            case 'v':
                bx_cli_print_version(progname);
                return 0;
            case '?':
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                return 1;
            default:
                return 1;
        }
    }

    FILE* in = stdin;
    const char* prefix = "x";
    if (optind < argc) {
        if (strcmp(argv[optind], "-") != 0) {
            in = fopen(argv[optind], "r");
            if (!in) {
                bx_perror(argv[optind]);
                return 1;
            }
        }
        optind++;
    }
    if (optind < argc) {
        prefix = argv[optind];
        optind++;
    }
    if (optind < argc) {
        bx_cli_diag_extra_operand(&diag, argv[optind]);
        if (in != stdin) {
            fclose(in);
        }
        return 1;
    }

    char* suffix = malloc((size_t)suffix_length + 1);
    memset(suffix, numeric || hex ? '0' : 'a', (size_t)suffix_length);
    suffix[suffix_length] = '\0';

    char* out_name = malloc(strlen(prefix) + (size_t)suffix_length + 1);

    bool eof = false;
    while (!eof) {
        sprintf(out_name, "%s%s", prefix, suffix);
        FILE* out = fopen(out_name, "w");
        if (!out) {
            bx_perror(out_name);
            break;
        }

        bool wrote_anything = false;

        if (split_by_lines) {
            for (long long i = 0; i < lines_per_file; i++) {
                int ch;
                while ((ch = getc(in)) != EOF) {
                    wrote_anything = true;
                    putc(ch, out);
                    if (ch == '\n')
                        break;
                }
                if (ch == EOF) {
                    eof = true;
                    break;
                }
            }
        }
        else {
            for (long long i = 0; i < split_size; i++) {
                int ch = getc(in);
                if (ch == EOF) {
                    eof = true;
                    break;
                }
                wrote_anything = true;
                putc(ch, out);
            }
        }
        fclose(out);

        if (!wrote_anything) {
            unlink(out_name);
        }

        if (eof)
            break;
        next_suffix(suffix, suffix_length, numeric, hex);
    }

    free(suffix);
    free(out_name);
    if (in != stdin)
        fclose(in);

    return 0;
}
