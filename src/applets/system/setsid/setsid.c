#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/child_runner.h"
#include "lib/option_scan.h"

struct bx_setsid_options {
    const char* progname;
    bool show_help;
    bool show_version;
    int command_index;
};

static void bx_setsid_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... COMMAND [ARG]...\n", progname);
    fprintf(stream, "Run COMMAND in a new session.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static bool bx_setsid_parse_options(int argc, char** argv, struct bx_setsid_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "setsid");
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result result = bx_option_next(&cursor);
    if (result.event == BX_OPTION_END) {
        options->command_index = (int)result.index;
        return true;
    }
    if (result.event == BX_OPTION_MATCH) {
        options->show_help = result.spec->id == 'h';
        options->show_version = result.spec->id == 'V';
        return true;
    }

    if (result.short_name)
        bx_diag(diag, "invalid option -- '%c'", result.short_name);
    else
        bx_diag(diag, "unrecognized option '%s'", result.token);
    return false;
}

int bx_setsid_main(int argc, char** argv) {
    struct bx_setsid_options options;
    struct bx_diag_ctx diag = {
        .progname = "setsid",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_setsid_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_setsid_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (options.command_index >= argc) {
        bx_diag(&diag, "missing command operand");
        bx_cli_print_try_help(options.progname);
        return 1;
    }
    if (argv[options.command_index][0] == '\0') {
        bx_diag(&diag, "empty command operand");
        return 127;
    }

    if (setsid() == -1) {
        bx_diag(&diag, "failed to create new session: %s", strerror(errno));
        return 1;
    }

    int exec_error = bx_child_exec_argv(argv + options.command_index);
    bx_diag(&diag, "failed to run command '%s': %s", argv[options.command_index], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}
