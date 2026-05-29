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
#include <errno.h>
#include <sys/stat.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

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
        res->chars++;

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

static int count_digits(unsigned long long n) {
    if (n == 0) return 1;
    int d = 0;
    while (n) { d++; n /= 10; }
    return d;
}

static void value_str(unsigned long long n, char* buf) {
    sprintf(buf, "%llu", n);
}

static void print_one(const char* fmt, int width, unsigned long long val) {
    char buf[32];
    value_str(val, buf);
    printf(fmt, width, buf);
}

static int compute_number_width(int num_files, char** files, int first_file_index) {
    int width = 1;
    int minimum_width = 1;
    unsigned long long regular_total = 0;
    bool any_non_regular = false;

    for (int i = 0; i < num_files; i++) {
        const char* name = files[first_file_index + i];
        if (strcmp(name, "-") == 0) {
            any_non_regular = true;
            continue;
        }
        struct stat st;
        if (stat(name, &st) != 0) {
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            any_non_regular = true;
        }
        else {
            if (regular_total + (unsigned long long)st.st_size < regular_total) {
                regular_total = ~0ULL;
                break;
            }
            regular_total += (unsigned long long)st.st_size;
        }
    }

    if (any_non_regular)
        minimum_width = 7;

    width = count_digits(regular_total);
    if (width < minimum_width)
        width = minimum_width;

    return width;
}

int bx_wc_main(int argc, char** argv) {
    static const struct option long_options[] = {{"bytes", no_argument, NULL, 'c'},           {"chars", no_argument, NULL, 'm'}, {"lines", no_argument, NULL, 'l'},   {"words", no_argument, NULL, 'w'},
                                                  {"max-line-length", no_argument, NULL, 'L'}, {"help", no_argument, NULL, 'h'},  {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "wc");
    struct bx_diag_ctx diag = {.progname = progname, .exit_status = 0};
    bool opt_c = false, opt_m = false, opt_l = false, opt_w = false, opt_L = false;
    int c;
    bx_args_getopt_reset();
    while ((c = bx_args_getopt_long(argc, argv, "cmwlL", long_options, NULL)) != -1) {
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
                printf("Usage: %s [OPTION]... [FILE]...\n", progname);
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
                bx_cli_print_version(progname);
                return 0;
            default:
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                return 1;
        }
    }

    if (!(opt_c || opt_m || opt_l || opt_w || opt_L)) {
        opt_l = opt_w = opt_c = true;
    }

    int num_files = argc - optind;
    int field_count = (opt_l ? 1 : 0) + (opt_w ? 1 : 0) + (opt_c ? 1 : 0) + (opt_m ? 1 : 0) + (opt_L ? 1 : 0);

    if (num_files == 0) {
        int width = field_count > 1 ? 7 : 1;
        wc_counts_t res;
        wc_count(stdin, &res);

        if (field_count > 1) {
            const char* fmt = "%*s";
            if (opt_l) { print_one(fmt, width, res.lines); fmt = " %*s"; }
            if (opt_w) { print_one(fmt, width, res.words); fmt = " %*s"; }
            if (opt_m) { print_one(fmt, width, res.chars); fmt = " %*s"; }
            if (opt_c) { print_one(fmt, width, res.bytes); fmt = " %*s"; }
            if (opt_L) { print_one(fmt, width, res.max_line_width); }
        } else {
            if (opt_l) printf("%llu", res.lines);
            if (opt_w) printf("%llu", res.words);
            if (opt_m) printf("%llu", res.chars);
            if (opt_c) printf("%llu", res.bytes);
            if (opt_L) printf("%llu", res.max_line_width);
        }
        printf("\n");
        return 0;
    }

    wc_counts_t* counts = malloc((size_t)num_files * sizeof(wc_counts_t));
    bool* file_ok = malloc((size_t)num_files * sizeof(bool));
    int ncounts = 0;
    for (int i = 0; i < num_files; i++) {
        const char* name = argv[optind + i];
        FILE* f;
        if (strcmp(name, "-") == 0) {
            f = stdin;
        }
        else {
            f = fopen(name, "r");
            if (!f) {
                bx_perror_path(&diag, name);
                file_ok[i] = false;
                continue;
            }
        }
        wc_count(f, &counts[ncounts]);
        if (f != stdin)
            fclose(f);
        file_ok[i] = true;
        ncounts++;
    }

    wc_counts_t total = {0};
    for (int i = 0; i < ncounts; i++) {
        total.lines += counts[i].lines;
        total.words += counts[i].words;
        total.chars += counts[i].chars;
        total.bytes += counts[i].bytes;
        if (counts[i].max_line_width > total.max_line_width)
            total.max_line_width = counts[i].max_line_width;
    }

    bool show_total = num_files > 1;
    int width = compute_number_width(num_files, argv, optind);

    int out_idx = 0;
    for (int i = 0; i < num_files; i++) {
        if (!file_ok[i])
            continue;
        wc_counts_t* r = &counts[out_idx++];

        const char* fmt = "%*s";
        if (opt_l) { print_one(fmt, width, r->lines); fmt = " %*s"; }
        if (opt_w) { print_one(fmt, width, r->words); fmt = " %*s"; }
        if (opt_m) { print_one(fmt, width, r->chars); fmt = " %*s"; }
        if (opt_c) { print_one(fmt, width, r->bytes); fmt = " %*s"; }
        if (opt_L) { print_one(fmt, width, r->max_line_width); }

        printf(" %s\n", argv[optind + i]);
    }

    if (show_total) {
        const char* fmt = "%*s";
        if (opt_l) { print_one(fmt, width, total.lines); fmt = " %*s"; }
        if (opt_w) { print_one(fmt, width, total.words); fmt = " %*s"; }
        if (opt_m) { print_one(fmt, width, total.chars); fmt = " %*s"; }
        if (opt_c) { print_one(fmt, width, total.bytes); fmt = " %*s"; }
        if (opt_L) { print_one(fmt, width, total.max_line_width); }
        printf(" total\n");
    }

    free(counts);
    free(file_ok);
    return diag.exit_status;
}
