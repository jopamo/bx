#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include <string.h>
#include "applets.h"
#include "diag.h"

int bx_whoami_main(int argc, char** argv) {
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
        bx_err("extra operand '%s'", argv[1]);
        return 1;
    }

    uid_t euid = geteuid();
    struct passwd* pwd = getpwuid(euid);
    if (pwd) {
        puts(pwd->pw_name);
        return 0;
    }
    bx_err("cannot find name for user ID %u", (unsigned int)euid);
    return 1;
}
