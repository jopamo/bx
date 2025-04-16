#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>
#include "applets.h"
#include "diag.h"

int bx_tee_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"append", no_argument, NULL, 'a'}, {"ignore-interrupts", no_argument, NULL, 'i'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    bool append = false;
    bool ignore_interrupts = false;
    int c;
    while ((c = getopt_long(argc, argv, "ai", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                append = true;
                break;
            case 'i':
                ignore_interrupts = true;
                break;
            case 'h':
                printf("Usage: %s [OPTION]... [FILE]...\n", argv[0]);
                printf("Copy standard input to each FILE, and also to standard output.\n");
                printf("\n");
                printf("  -a, --append             append to the given FILEs, do not overwrite\n");
                printf("  -i, --ignore-interrupts  ignore interrupt signals\n");
                printf("      --help          display this help and exit\n");
                printf("      --version       output version information and exit\n");
                return 0;
            case 'v':
                printf("tee (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (ignore_interrupts) {
        signal(SIGINT, SIG_IGN);
    }

    int num_files = argc - optind;
    FILE** files = malloc((size_t)(num_files + 1) * sizeof(FILE*));
    files[0] = stdout;
    int actual_files = 1;

    for (int i = 0; i < num_files; i++) {
        FILE* f = fopen(argv[optind + i], append ? "a" : "w");
        if (f) {
            files[actual_files++] = f;
        }
        else {
            bx_perror(argv[optind + i]);
        }
    }

    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
        for (int i = 0; i < actual_files; i++) {
            fwrite(buf, 1, n, files[i]);
            fflush(files[i]);
        }
    }

    for (int i = 1; i < actual_files; i++) {
        fclose(files[i]);
    }
    free(files);

    return 0;
}
