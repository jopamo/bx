#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <string.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

int bx_whoami_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "whoami"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s [OPTION]...\n", diag.progname);
            printf("Print the user name associated with the current effective user ID.\n");
            printf("Same as id -un.\n");
            printf("\n");
            printf("      --help          display this help and exit\n");
            printf("      --version       output version information and exit\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            bx_cli_print_version(diag.progname);
            return 0;
        }
        bx_diag(&diag, "extra operand '%s'", argv[1]);
        bx_cli_print_try_help(diag.progname);
        return 1;
    }

    uid_t euid = geteuid();
    struct passwd* pwd = getpwuid(euid);
    if (pwd) {
        puts(pwd->pw_name);
        return 0;
    }
    bx_diag(&diag, "cannot find name for user ID %u", (unsigned int)euid);
    return 1;
}
