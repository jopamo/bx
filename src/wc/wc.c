#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <wchar.h>
#include <wctype.h>
#include <locale.h>
#include <stdbool.h>
#include <ctype.h>
#include "applets.h"
#include "diag.h"

typedef struct {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long chars;
    unsigned long long bytes;
    unsigned long long max_line_width;
} wc_counts_t;

static void wc_count(FILE* f, wc_counts_t* res) {
    memset(res, 0, sizeof(*res));
    unsigned long long current_line_width = 0;
    bool in_word = false;

    int c;
    while ((c = getc(f)) != EOF) {
        res->bytes++;
        res->chars++;  // Simple ASCII/UTF-8 byte-as-char for now, can be improved with mbrtowc

        if (c == '\n') {
            res->lines++;
            if (current_line_width > res->max_line_width) {
                res->max_line_width = current_line_width;
            }
            current_line_width = 0;
        }
        else if (c == '\t') {
            current_line_width += 8 - (current_line_width % 8);
        }
        else if (isprint(c)) {
            current_line_width++;
        }

        if (isspace(c)) {
            in_word = false;
        }
        else if (!in_word) {
            in_word = true;
            res->words++;
        }
    }
    if (current_line_width > res->max_line_width) {
        res->max_line_width = current_line_width;
    }
}

static void print_counts(const wc_counts_t* res, bool opt_l, bool opt_w, bool opt_c, bool opt_m, bool opt_L, const char* name) {
    bool first = true;
    if (opt_l) {
        printf("%s%llu", first ? "" : " ", res->lines);
        first = false;
    }
    if (opt_w) {
        printf("%s%llu", first ? "" : " ", res->words);
        first = false;
    }
    if (opt_m) {
        printf("%s%llu", first ? "" : " ", res->chars);
        first = false;
    }
    if (opt_c) {
        printf("%s%llu", first ? "" : " ", res->bytes);
        first = false;
    }
    if (opt_L) {
        printf("%s%llu", first ? "" : " ", res->max_line_width);
        first = false;
    }
    if (name)
        printf(" %s", name);
    printf("\n");
}

int bx_wc_main(int argc, char** argv) {
    static const struct option long_options[] = {{"bytes", no_argument, NULL, 'c'},           {"chars", no_argument, NULL, 'm'}, {"lines", no_argument, NULL, 'l'},   {"words", no_argument, NULL, 'w'},
                                                 {"max-line-length", no_argument, NULL, 'L'}, {"help", no_argument, NULL, 'h'},  {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    bool opt_c = false, opt_m = false, opt_l = false, opt_w = false, opt_L = false;
    int c;
    while ((c = getopt_long(argc, argv, "cmwlL", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                opt_c = true;
                break;
            case 'm':
                opt_m = true;
                break;
            case 'l':
                opt_l = true;
                break;
            case 'w':
                opt_w = true;
                break;
            case 'L':
                opt_L = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Print newline, word, and byte counts for each FILE, and a total line if\n");
                printf("more than one FILE is specified.  A word is a non-zero-length sequence of\n");
                printf("characters delimited by white space.\n");
                printf("\n");
                printf("  -c, --bytes            print the byte counts\n");
                printf("  -m, --chars            print the character counts\n");
                printf("  -l, --lines            print the newline counts\n");
                printf("  -L, --max-line-length  print the maximum display width\n");
                printf("  -w, --words            print the word counts\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("wc (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (!(opt_c || opt_m || opt_l || opt_w || opt_L)) {
        opt_l = opt_w = opt_c = true;
    }

    wc_counts_t total = {0};
    int files_processed = 0;

    if (optind == argc) {
        wc_counts_t res;
        wc_count(stdin, &res);
        print_counts(&res, opt_l, opt_w, opt_c, opt_m, opt_L, NULL);
        return 0;
    }

    for (int i = optind; i < argc; i++) {
        FILE* f;
        const char* name = argv[i];
        if (strcmp(name, "-") == 0) {
            f = stdin;
            name = NULL;
        }
        else {
            f = fopen(name, "r");
            if (!f) {
                bx_perror(argv[i]);
                continue;
            }
        }

        wc_counts_t res;
        wc_count(f, &res);
        if (f != stdin)
            fclose(f);

        print_counts(&res, opt_l, opt_w, opt_c, opt_m, opt_L, argv[i]);

        total.lines += res.lines;
        total.words += res.words;
        total.chars += res.chars;
        total.bytes += res.bytes;
        if (res.max_line_width > total.max_line_width) {
            total.max_line_width = res.max_line_width;
        }
        files_processed++;
    }

    if (files_processed > 1) {
        print_counts(&total, opt_l, opt_w, opt_c, opt_m, opt_L, "total");
    }

    return 0;
}
