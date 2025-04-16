#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include "applets.h"
#include "diag.h"

static int expand_set(const char* arg, unsigned char* out) {
    int len = 0;
    for (const char* p = arg; *p; p++) {
        if (p[0] == '\\' && p[1]) {
            p++;
            switch (*p) {
                case 'n':
                    out[len++] = '\n';
                    break;
                case 't':
                    out[len++] = '\t';
                    break;
                case 'r':
                    out[len++] = '\r';
                    break;
                case '\\':
                    out[len++] = '\\';
                    break;
                default:
                    out[len++] = *p;
                    break;
            }
        }
        else if (p[1] == '-' && p[2]) {
            unsigned char start = p[0];
            unsigned char end = p[2];
            for (int i = start; i <= end; i++) {
                out[len++] = (unsigned char)i;
            }
            p += 2;
        }
        else {
            out[len++] = *p;
        }
    }
    return len;
}

int bx_tr_main(int argc, char** argv) {
    static const struct option long_options[] = {{"complement", no_argument, NULL, 'c'},
                                                 {"delete", no_argument, NULL, 'd'},
                                                 {"squeeze-repeats", no_argument, NULL, 's'},
                                                 {"truncate-set1", no_argument, NULL, 't'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'v'},
                                                 {NULL, 0, NULL, 0}};

    bool complement = false, delete = false, squeeze = false, truncate = false;
    int c;
    while ((c = getopt_long(argc, argv, "cdst", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                complement = true;
                break;
            case 'd':
                delete = true;
                break;
            case 's':
                squeeze = true;
                break;
            case 't':
                truncate = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... SET1 [SET2]\n", argv[0]);
                // ...
                return 0;
            case 'v':
                printf("tr (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (optind >= argc) {
        bx_err("missing operand");
        return 1;
    }

    unsigned char set1[1024], set2[1024];
    int len1 = expand_set(argv[optind], set1);
    int len2 = 0;
    if (optind + 1 < argc) {
        len2 = expand_set(argv[optind + 1], set2);
    }

    unsigned char map[256];
    bool in_set1[256] = {0};
    bool del[256] = {0};
    for (int i = 0; i < 256; i++)
        map[i] = (unsigned char)i;

    if (delete) {
        for (int i = 0; i < len1; i++)
            del[set1[i]] = true;
        if (complement) {
            for (int i = 0; i < 256; i++)
                del[i] = !del[i];
        }
    }
    else {
        if (len2 == 0) {
            bx_err("missing operand after '%s'", argv[optind]);
            return 1;
        }
        for (int i = 0; i < len1; i++) {
            map[set1[i]] = (i < len2) ? set2[i] : set2[len2 - 1];
        }
    }

    int prev_c = -1;
    int ch;
    while ((ch = getchar()) != EOF) {
        if (del[ch])
            continue;
        unsigned char out_c = map[ch];
        if (squeeze) {
            bool in_squeeze_set = false;
            // For now, if no delete, squeeze SET2, else squeeze SET1
            // GNU: "squeeze-repeats replaces each sequence of a repeated character that is listed in the last specified SET"
            const char* squeeze_arg = (optind + 1 < argc) ? argv[optind + 1] : argv[optind];
            unsigned char sset[1024];
            int slen = expand_set(squeeze_arg, sset);
            for (int i = 0; i < slen; i++) {
                if (out_c == sset[i]) {
                    in_squeeze_set = true;
                    break;
                }
            }
            if (in_squeeze_set && out_c == prev_c)
                continue;
        }
        putchar(out_c);
        prev_c = out_c;
    }

    return 0;
}
