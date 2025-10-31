#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_prtstat_reference_main(int argc, char** argv);

static void bx_prtstat_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... PID...\n", progname);
    fprintf(stream, "Print decoded /proc/PID/stat information.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -r, --raw              dump raw stat fields\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

int bx_prtstat_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "prtstat", "-h", bx_prtstat_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_prtstat_reference_main(argc, argv);
}
