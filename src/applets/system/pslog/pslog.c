#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_pslog_reference_main(int argc, const char* argv[]);

static void bx_pslog_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s PID...\n", progname);
    fprintf(stream, "Print log file paths currently opened by each PID.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

int bx_pslog_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "pslog", "-h", bx_pslog_print_help);
    if (handled >= 0) {
        return handled;
    }

    const char* const_argv[argc > 0 ? argc : 1];
    for (int i = 0; i < argc; i++) {
        const_argv[i] = argv[i];
    }

    return bx_pslog_reference_main(argc, const_argv);
}
