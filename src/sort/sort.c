#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include "applets.h"
#include "diag.h"

typedef struct {
    bool numeric;
    bool reverse;
    bool unique;
    bool ignore_case;
} sort_opts_t;

static sort_opts_t global_opts;

static int compare_lines(const void* a, const void* b) {
    const char* s1 = *(const char* const*)a;
    const char* s2 = *(const char* const*)b;
    int res;

    if (global_opts.numeric) {
        long long v1 = atoll(s1);
        long long v2 = atoll(s2);
        if (v1 < v2)
            res = -1;
        else if (v1 > v2)
            res = 1;
        else
            res = 0;
    }
    else if (global_opts.ignore_case) {
        res = strcasecmp(s1, s2);
    }
    else {
        res = strcmp(s1, s2);
    }

    return global_opts.reverse ? -res : res;
}

int bx_sort_main(int argc, char** argv) {
    static const struct option long_options[] = {{"numeric-sort", no_argument, NULL, 'n'},
                                                 {"reverse", no_argument, NULL, 'r'},
                                                 {"unique", no_argument, NULL, 'u'},
                                                 {"ignore-case", no_argument, NULL, 'f'},
                                                 {"help", no_argument, NULL, 'h'},
                                                 {"version", no_argument, NULL, 'v'},
                                                 {NULL, 0, NULL, 0}};

    memset(&global_opts, 0, sizeof(global_opts));
    int c;
    while ((c = getopt_long(argc, argv, "nruf", long_options, NULL)) != -1) {
        switch (c) {
            case 'n':
                global_opts.numeric = true;
                break;
            case 'r':
                global_opts.reverse = true;
                break;
            case 'u':
                global_opts.unique = true;
                break;
            case 'f':
                global_opts.ignore_case = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                // ...
                return 0;
            case 'v':
                printf("sort (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    char** lines = NULL;
    size_t nlines = 0;
    size_t cap = 0;

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;

    for (int i = (optind == argc ? -1 : optind); i < argc; i++) {
        FILE* f;
        if (i == -1) {
            f = stdin;
        }
        else {
            if (strcmp(argv[i], "-") == 0)
                f = stdin;
            else {
                f = fopen(argv[i], "r");
                if (!f) {
                    bx_perror(argv[i]);
                    continue;
                }
            }
        }

        while ((len = getline(&line, &line_cap, f)) != -1) {
            if (nlines >= cap) {
                cap = cap ? cap * 2 : 1024;
                lines = realloc(lines, cap * sizeof(char*));
            }
            lines[nlines++] = strdup(line);
        }
        if (f != stdin)
            fclose(f);
        if (i == -1)
            break;
    }
    free(line);

    if (nlines > 0) {
        qsort(lines, nlines, sizeof(char*), compare_lines);

        for (size_t i = 0; i < nlines; i++) {
            if (global_opts.unique && i > 0 && compare_lines(&lines[i - 1], &lines[i]) == 0) {
                free(lines[i]);
                continue;
            }
            fputs(lines[i], stdout);
            free(lines[i]);
        }
    }
    free(lines);

    return 0;
}
