#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <ctype.h>
#include "applets.h"
#include "bx/diag.h"

typedef struct {
    bool count;
    bool repeated;
    bool unique;
    bool ignore_case;
    int skip_fields;
    int skip_chars;
    int check_chars;
    bool zero_terminated;
} uniq_opts_t;

static const char* skip_to_compare(const char* s, const uniq_opts_t* opts) {
    int f = opts->skip_fields;
    while (f > 0 && *s) {
        while (*s && isspace(*s))
            s++;
        while (*s && !isspace(*s))
            s++;
        f--;
    }
    int c = opts->skip_chars;
    while (c > 0 && *s) {
        s++;
        c--;
    }
    return s;
}

static int line_compare(const char* s1, const char* s2, const uniq_opts_t* opts) {
    s1 = skip_to_compare(s1, opts);
    s2 = skip_to_compare(s2, opts);

    if (opts->check_chars > 0) {
        if (opts->ignore_case) {
            return strncasecmp(s1, s2, opts->check_chars);
        }
        else {
            return strncmp(s1, s2, opts->check_chars);
        }
    }
    else {
        if (opts->ignore_case) {
            return strcasecmp(s1, s2);
        }
        else {
            return strcmp(s1, s2);
        }
    }
}

static void do_uniq(FILE* in, FILE* out, const uniq_opts_t* opts) {
    char *line = NULL, *prev_line = NULL;
    size_t line_cap = 0, prev_line_cap = 0;
    ssize_t line_len;
    unsigned long long count = 0;
    char delimiter = opts->zero_terminated ? '\0' : '\n';

    while ((line_len = getdelim(&line, &line_cap, delimiter, in)) != -1) {
        if (prev_line == NULL) {
            prev_line = malloc(line_cap);
            memcpy(prev_line, line, line_len + 1);
            prev_line_cap = line_cap;
            count = 1;
            continue;
        }

        if (line_compare(line, prev_line, opts) == 0) {
            count++;
        }
        else {
            bool should_print = true;
            if (opts->repeated && count == 1)
                should_print = false;
            if (opts->unique && count > 1)
                should_print = false;

            if (should_print) {
                if (opts->count)
                    fprintf(out, "%7llu ", count);
                fputs(prev_line, out);
            }

            if (line_cap > prev_line_cap) {
                prev_line = realloc(prev_line, line_cap);
                prev_line_cap = line_cap;
            }
            memcpy(prev_line, line, line_len + 1);
            count = 1;
        }
    }

    if (prev_line != NULL) {
        bool should_print = true;
        if (opts->repeated && count == 1)
            should_print = false;
        if (opts->unique && count > 1)
            should_print = false;

        if (should_print) {
            if (opts->count)
                fprintf(out, "%7llu ", count);
            fputs(prev_line, out);
        }
    }

    free(line);
    free(prev_line);
}

int bx_uniq_main(int argc, char** argv) {
    static const struct option long_options[] = {{"count", no_argument, NULL, 'c'},
                                                 {"repeated", no_argument, NULL, 'd'},
                                                 {"unique", no_argument, NULL, 'u'},
                                                 {"ignore-case", no_argument, NULL, 'i'},
                                                 {"skip-fields", required_argument, NULL, 'f'},
                                                 {"skip-chars", required_argument, NULL, 's'},
                                                 {"check-chars", required_argument, NULL, 'w'},
                                                 {"zero-terminated", no_argument, NULL, 'z'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'v'},
                                                 {NULL, 0, NULL, 0}};

    uniq_opts_t opts = {0};
    int c;
    while ((c = getopt_long(argc, argv, "cduif:s:w:z", long_options, NULL)) != -1) {
        switch (c) {
            case 'c':
                opts.count = true;
                break;
            case 'd':
                opts.repeated = true;
                break;
            case 'u':
                opts.unique = true;
                break;
            case 'i':
                opts.ignore_case = true;
                break;
            case 'f':
                opts.skip_fields = atoi(optarg);
                break;
            case 's':
                opts.skip_chars = atoi(optarg);
                break;
            case 'w':
                opts.check_chars = atoi(optarg);
                break;
            case 'z':
                opts.zero_terminated = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [INPUT [OUTPUT]]\n", argv[0]);
                // ...
                return 0;
            case 'v':
                printf("uniq (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    FILE *in = stdin, *out = stdout;
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
        out = fopen(argv[optind], "w");
        if (!out) {
            bx_perror(argv[optind]);
            return 1;
        }
        optind++;
    }

    do_uniq(in, out, &opts);

    if (in != stdin)
        fclose(in);
    if (out != stdout)
        fclose(out);

    return 0;
}
