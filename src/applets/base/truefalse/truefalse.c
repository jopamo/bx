#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "lib/cli_common.h"

static int bx_constant_status_main(int argc,
                                   char **argv,
                                   const char *fallback_progname,
                                   const char *status_word,
                                   int status) {
    const char *progname = bx_cli_progname((argc > 0) ? argv[0] : NULL,
                                           fallback_progname);

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        printf("Usage: %s [ignored command line arguments]\n", progname);
        printf("  or:  %s OPTION\n", progname);
        printf("Exit with a status code indicating %s.\n", status_word);
        puts("");
        puts("      --help     display this help and exit");
        puts("      --version  output version information and exit");
        return status;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        bx_cli_print_version(progname);
        return status;
    }

    return status;
}

int bx_true_main(int argc, char **argv) {
    return bx_constant_status_main(argc, argv, "true", "success", 0);
}

int bx_false_main(int argc, char **argv) {
    return bx_constant_status_main(argc, argv, "false", "failure", 1);
}
