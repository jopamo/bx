#include "lib/manual_runtime.h"

int bx_mandoc_main_impl(int argc, char **argv);

int bx_manual_runtime_main(
    int argc,
    char **argv,
    const char *fallback_progname,
    bx_cli_help_fn print_help
) {
    int handled = bx_cli_maybe_handle_help_or_version(
        argc,
        argv,
        fallback_progname,
        NULL,
        NULL,
        print_help
    );
    if (handled >= 0) {
        return handled;
    }

    return bx_mandoc_main_impl(argc, argv);
}
