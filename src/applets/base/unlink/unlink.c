#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"

static void bx_unlink_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s FILE\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Call the unlink function to remove the specified FILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

int bx_unlink_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "unlink"),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    int status = bx_cli_scan_help_version_prefix(argc, argv, &first_operand, &diag, bx_unlink_print_help);
    if (status >= 0)
        return status;

    int operand_count = argc - first_operand;
    char** operands = argv + first_operand;
    if (operand_count <= 0) {
        bx_cli_diag_missing_operand(&diag);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }

    if (operand_count > 1) {
        bx_cli_diag_extra_operand(&diag, operands[1]);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }

    if (bx_fd_unlinkat(AT_FDCWD, operands[0], 0) != 0) {
        bx_diag(&diag, "cannot unlink '%s': %s", operands[0], strerror(errno));
    }

    return diag.exit_status;
}
