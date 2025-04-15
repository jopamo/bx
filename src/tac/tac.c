#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include "applets.h"
#include "diag.h"

static void tac_file(FILE* f, const char* separator, bool before) {
    char** lines = NULL;
    size_t* line_lens = NULL;
    size_t nlines = 0;
    size_t cap = 0;

    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;

    // Default separator is newline
    char sep = '\n';
    if (separator && separator[0] != '\0' && separator[1] == '\0') {
        sep = separator[0];
    }

    while ((len = getdelim(&line, &line_cap, sep, f)) != -1) {
        if (nlines >= cap) {
            cap = cap ? cap * 2 : 1024;
            lines = realloc(lines, cap * sizeof(char*));
            line_lens = realloc(line_lens, cap * sizeof(size_t));
        }
        lines[nlines] = malloc(len + 1);
        memcpy(lines[nlines], line, len + 1);
        line_lens[nlines] = len;
        nlines++;
    }
    free(line);

    for (size_t i = nlines; i > 0; i--) {
        fwrite(lines[i - 1], 1, line_lens[i - 1], stdout);
        free(lines[i - 1]);
    }
    free(lines);
    free(line_lens);
}

int bx_tac_main(int argc, char** argv) {
    static const struct option long_options[] = {{"before", no_argument, NULL, 'b'}, {"regex", no_argument, NULL, 'r'},   {"separator", required_argument, NULL, 's'},
                                                 {"help", no_argument, NULL, 'h'},   {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    bool before = false;
    bool regex = false;
    const char* separator = "\n";
    int c;
    while ((c = getopt_long(argc, argv, "brs:", long_options, NULL)) != -1) {
        switch (c) {
            case 'b':
                before = true;
                break;
            case 'r':
                regex = true;
                break;
            case 's':
                separator = optarg;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Write each FILE to standard output, last line first.\n");
                printf("\n");
                printf("  -b, --before             attach the separator before instead of after\n");
                printf("  -r, --regex              interpret the separator as a regular expression\n");
                printf("  -s, --separator=STRING   use STRING as the separator instead of newline\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("tac (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (regex) {
        bx_err("--regex is not yet supported");
        return 1;
    }

    if (optind == argc) {
        tac_file(stdin, separator, before);
    }
    else {
        for (int i = optind; i < argc; i++) {
            FILE* f;
            if (strcmp(argv[i], "-") == 0) {
                f = stdin;
            }
            else {
                f = fopen(argv[i], "r");
                if (!f) {
                    bx_perror(argv[i]);
                    continue;
                }
            }
            tac_file(f, separator, before);
            if (f != stdin)
                fclose(f);
        }
    }

    return 0;
}
