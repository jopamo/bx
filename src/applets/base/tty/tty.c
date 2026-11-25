#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

static void bx_tty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the file name of the terminal connected to standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --silent, --quiet\n");
    fprintf(stream, "         print nothing, only return an exit status\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
}

static int bx_tty_emit_state(bool silent, bool stdin_is_tty, const char* tty_name, struct bx_diag_ctx* diag) {
    if (silent) {
        return stdin_is_tty ? 0 : 1;
    }

    if (stdin_is_tty) {
        if (!bx_cli_emit_line(tty_name, false, diag)) {
            return diag->exit_status;
        }
    }
    else {
        if (!bx_cli_emit_line("not a tty", false, diag)) {
            return diag->exit_status;
        }
    }

    if (!bx_cli_flush_stdout(diag)) {
        return diag->exit_status;
    }

    return stdin_is_tty ? 0 : 1;
}

int bx_tty_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'s', 's', "silent", BX_OPTION_NO_VALUE},
        {'s', 0, "quiet", BX_OPTION_NO_VALUE},
        {1, 0, "help", BX_OPTION_NO_VALUE},
        {2, 0, "version", BX_OPTION_NO_VALUE},
    };
    struct bx_diag_ctx diag = {.progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "tty")};
    bool silent = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(diag.progname);
            return 2;
        }
        if (option.spec->id == 1) {
            bx_tty_print_help(stdout, diag.progname);
            return 0;
        }
        if (option.spec->id == 2) {
            bx_cli_print_version(diag.progname);
            return 0;
        }
        silent = true;
    }

    if (option.index < (size_t)argc) {
        bx_cli_diag_extra_operand(&diag, argv[option.index]);
        bx_cli_print_try_help(diag.progname);
        return 2;
    }

    const char* tty_name = ttyname(STDIN_FILENO);
    return bx_tty_emit_state(silent, tty_name != NULL, tty_name, &diag);
}
