#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <string.h>
#include "applets.h"
#include "bx/diag.h"

static const char* bx_whoami_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "whoami";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

int bx_whoami_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_whoami_progname((argc > 0) ? argv[0] : NULL),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (argc > 1) {
        if (strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s [OPTION]...\n", argv[0]);
            printf("Print the user name associated with the current effective user ID.\n");
            printf("Same as id -un.\n");
            printf("\n");
            printf("      --help          display this help and exit\n");
            printf("      --version       output version information and exit\n");
            return 0;
        }
        if (strcmp(argv[1], "--version") == 0) {
            printf("whoami (bx) %s\n", BX_VERSION);
            return 0;
        }
        bx_diag(&diag, "extra operand '%s'", argv[1]);
        fprintf(stderr, "Try '%s --help' for more information.\n", diag.progname);
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
