#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include "applets.h"
#include "bx/diag.h"

static void unexpand_file(FILE* f, int tab_size, bool all) {
    int c;
    int column = 0;
    int pending_spaces = 0;
    bool initial_blanks = true;

    while ((c = getc(f)) != EOF) {
        if (c == ' ' && (all || initial_blanks)) {
            pending_spaces++;
            column++;
            if (column % tab_size == 0) {
                if (pending_spaces > 0) {
                    putchar('\t');
                    pending_spaces = 0;
                }
            }
        }
        else {
            while (pending_spaces > 0) {
                putchar(' ');
                pending_spaces--;
            }
            putchar(c);
            if (c == '\n') {
                column = 0;
                initial_blanks = true;
            }
            else if (c == '\t') {
                column += tab_size - (column % tab_size);
            }
            else {
                column++;
                if (c != ' ' && c != '\t')
                    initial_blanks = false;
            }
        }
    }
    while (pending_spaces > 0) {
        putchar(' ');
        pending_spaces--;
    }
}

int bx_unexpand_main(int argc, char** argv) {
    static const struct option long_options[] = {{"all", no_argument, NULL, 'a'},  {"first-only", no_argument, NULL, 1}, {"tabs", required_argument, NULL, 't'},
                                                 {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'},  {NULL, 0, NULL, 0}};

    bool all = false;
    int tab_size = 8;
    int c;
    while ((c = getopt_long(argc, argv, "at:", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                all = true;
                break;
            case 1:
                all = false;
                break;  // --first-only
            case 't':
                tab_size = atoi(optarg);
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Convert spaces in each FILE to tabs, writing to standard output.\n");
                printf("\n");
                printf("  -a, --all                convert all blanks, instead of just initial blanks\n");
                printf("      --first-only         convert only leading sequences of blanks (overrides -a)\n");
                printf("  -t, --tabs=N             have tabs N characters apart instead of 8\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("unexpand (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (tab_size <= 0)
        tab_size = 8;

    if (optind == argc) {
        unexpand_file(stdin, tab_size, all);
    }
    else {
        for (int i = optind; i < argc; i++) {
            FILE* f = fopen(argv[i], "r");
            if (!f) {
                bx_perror(argv[i]);
                continue;
            }
            unexpand_file(f, tab_size, all);
            fclose(f);
        }
    }

    return 0;
}
