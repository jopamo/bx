#include <stdio.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_logname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]\n", progname);
    fprintf(stream, "Print the user's login name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

int bx_logname_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "logname"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int operand = argc;
    int status = bx_cli_scan_help_version_prefix(argc, argv, &operand, &diag, bx_logname_print_help);
    if (status >= 0)
        return status;
    if (operand < argc) {
        bx_cli_diag_extra_operand(&diag, argv[operand]);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }

    const char* login = getlogin();
    if (login == NULL) {
        bx_diag(&diag, "no login name");
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    printf("%s\n", login);

    return 0;
}
