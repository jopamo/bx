#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_peekfd_reference_main(int argc, char** argv);

static void bx_peekfd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-8] [-n] [-c] [-t] [-d] PID [FD ...]\n", progname);
    fprintf(stream, "Trace read/write traffic on the selected process file descriptors.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -8, --eight-bit-clean      print raw 8-bit data\n");
    fprintf(stream, "  -c, --follow               follow forked child processes\n");
    fprintf(stream, "  -d, --duplicates-removed   suppress duplicate reads and writes\n");
    fprintf(stream, "  -n, --no-headers           omit per-read/write headers\n");
    fprintf(stream, "  -t, --tgid                 attach to all threads in the thread group\n");
    fprintf(stream, "  -h, --help                 display this help and exit\n");
    fprintf(stream, "  -V, --version              output version information and exit\n");
}

int bx_peekfd_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "peekfd", "-h", bx_peekfd_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_peekfd_reference_main(argc, argv);
}
