#include <stdio.h>
#include <string.h>

#include "applets/system/psmisc/psmisc_wrapper.h"
#include "lib/cli_common.h"

const char* bx_psmisc_progname(const char* argv0, const char* fallback) {
    return bx_cli_progname(argv0, fallback);
}

int bx_psmisc_maybe_handle_help_or_version(int argc, char** argv, const char* fallback,
                                           const char* short_help_opt, bx_psmisc_help_fn print_help) {
    const char* progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, fallback);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (arg == NULL) {
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            break;
        }

        if (strcmp(arg, "--help") == 0 || (short_help_opt != NULL && strcmp(arg, short_help_opt) == 0)) {
            print_help(stdout, progname);
            return 0;
        }

        if (strcmp(arg, "--version") == 0 || strcmp(arg, "-V") == 0) {
            bx_cli_print_version(progname);
            return 0;
        }
    }

    return -1;
}
