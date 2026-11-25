#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_hostname_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [NAME]\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Print or set the current system hostname.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With NAME, set the hostname to NAME.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static int bx_hostname_print_current(struct bx_diag_ctx* diag) {
    struct utsname uts;

    if (uname(&uts) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "%s: cannot determine hostname: %s\n", diag->progname, strerror(saved_errno));
        return 1;
    }

    if (!bx_cli_emit_line(uts.nodename, false, diag)) {
        return diag->exit_status;
    }

    if (!bx_cli_flush_stdout(diag)) {
        return diag->exit_status;
    }

    return 0;
}

static int bx_hostname_set_name(const char* name, struct bx_diag_ctx* diag) {
    if (sethostname(name, strlen(name)) != 0) {
        int saved_errno = errno;
        fprintf(stderr, "%s: cannot set name to '%s': %s\n", diag->progname, name, strerror(saved_errno));
        return 1;
    }

    return 0;
}

int bx_hostname_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "hostname"),
        .exit_status = 1,
    };

    int operand;
    int rc = bx_cli_scan_help_version_prefix(argc, argv, &operand, &diag, bx_hostname_print_help);
    if (rc >= 0)
        return rc;

    if (argc - operand > 1) {
        bx_cli_diag_extra_operand(&diag, argv[operand + 1]);
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status;
    }

    if (operand < argc) {
        return bx_hostname_set_name(argv[operand], &diag);
    }

    return bx_hostname_print_current(&diag);
}
