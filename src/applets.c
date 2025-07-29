#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "applets.h"

int bx_true_main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [ignored command line arguments]\n", argv[0]);
        printf("  or:  %s OPTION\n", argv[0]);
        puts("Exit with a status code indicating success.");
        puts("");
        puts("      --help     display this help and exit");
        puts("      --version  output version information and exit");
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("true (bx) %s\n", BX_VERSION);
        return 0;
    }

    (void)argc;
    (void)argv;
    return 0;
}

int bx_false_main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [ignored command line arguments]\n", argv[0]);
        printf("  or:  %s OPTION\n", argv[0]);
        puts("Exit with a status code indicating failure.");
        puts("");
        puts("      --help     display this help and exit");
        puts("      --version  output version information and exit");
        return 1;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("false (bx) %s\n", BX_VERSION);
        return 1;
    }

    (void)argc;
    (void)argv;
    return 1;
}
