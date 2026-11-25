#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_whoami_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the user name associated with the current effective user ID.\n");
    fprintf(stream, "Same as id -un.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

int bx_whoami_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "whoami"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    /* Exact help and version spellings precede surplus operands for whoami. */
    int status = bx_cli_maybe_handle_help_or_version(argc, argv, "whoami", NULL, NULL, bx_whoami_print_help);
    if (status >= 0)
        return status;

    int operand = argc;
    status = bx_cli_scan_help_version_prefix(argc, argv, &operand, &diag, bx_whoami_print_help);
    if (status >= 0)
        return status;
    if (operand < argc) {
        bx_cli_diag_extra_operand(&diag, argv[operand]);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
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
