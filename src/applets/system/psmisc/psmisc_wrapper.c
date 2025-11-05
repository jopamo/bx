#include <stdio.h>
#include <string.h>

#include "applets/system/psmisc/psmisc_wrapper.h"
#include "lib/cli_common.h"

const char* bx_psmisc_progname(const char* argv0, const char* fallback) {
    return bx_cli_progname(argv0, fallback);
}

int bx_psmisc_maybe_handle_help_or_version(int argc, char** argv, const char* fallback,
                                           const char* short_help_opt, bx_psmisc_help_fn print_help) {
    return bx_cli_maybe_handle_help_or_version(
        argc,
        argv,
        fallback,
        short_help_opt,
        "-V",
        print_help
    );
}
