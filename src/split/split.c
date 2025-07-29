#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "diag.h"

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

int bx_split_main(int argc, char** argv) {
    static const struct option long_options[] = {{"bytes", required_argument, NULL, 'b'},
                                                 {"lines", required_argument, NULL, 'l'},
                                                 {"suffix-length", required_argument, NULL, 'a'},
                                                 {"numeric-suffixes", optional_argument, NULL, 'd'},
                                                 {"hex-suffixes", optional_argument, NULL, 'x'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'v'},
                                                 {NULL, 0, NULL, 0}};

    long long split_size = 0;
    bool split_by_lines = true;
    long long lines_per_file = 1000;
    int suffix_length = 2;
    bool numeric = false;
    bool hex = false;

    int c;
    while ((c = getopt_long(argc, argv, "b:l:a:dx", long_options, NULL)) != -1) {
        switch (c) {
            case 'b':
                split_size = atoll(optarg);  // Should handle suffixes like K, M
                split_by_lines = false;
                break;
            case 'l':
                lines_per_file = atoll(optarg);
                split_by_lines = true;
                break;
            case 'a':
                suffix_length = atoi(optarg);
                break;
            case 'd':
                numeric = true;
                break;
            case 'x':
                hex = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE [PREFIX]]\n", argv[0]);
                // ...
                return 0;
            case 'v':
                printf("split (bx) %s\n", BX_VERSION);
                return 0;
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
