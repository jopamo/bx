#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"
#include "lib/args_common.h"

extern char** environ;

struct bx_printenv_options {
    const char* progname;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_printenv_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [VARIABLE]...\n", progname);
    fprintf(stream, "Print the values of environment VARIABLE(s).\n");
    fprintf(stream, "If no VARIABLE is specified, print name and value pairs for all.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -0, --null     end each output line with NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_printenv_parse_options(int argc, char** argv, struct bx_printenv_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"null", no_argument, NULL, '0'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "printenv");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+0", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '0':
                options->zero_terminated = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static bool bx_printenv_write_error(struct bx_diag_ctx* diag) {
    int saved_errno = errno != 0 ? errno : EIO;
    bx_diag(diag, "write error: %s", strerror(saved_errno));
    errno = saved_errno;
    return false;
}

static bool bx_printenv_emit_delimited(struct bx_line_writer* writer, const char* value, int delimiter, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_write(writer, value, strlen(value)) ||
        !bx_line_writer_putc(writer, (char)delimiter)) {
        return bx_printenv_write_error(diag);
    }

    return true;
}

static bool bx_printenv_print_all(struct bx_line_writer* writer, bool zero_terminated, struct bx_diag_ctx* diag) {
    int delimiter = zero_terminated ? '\0' : '\n';

    for (char** entry = environ; entry != NULL && *entry != NULL; entry++) {
        if (!bx_printenv_emit_delimited(writer, *entry, delimiter, diag)) {
            return false;
        }
    }

    return true;
}

int bx_printenv_main(int argc, char** argv) {
    struct bx_printenv_options options;
    struct bx_diag_ctx diag = {
        .progname = "printenv",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_printenv_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_printenv_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    bool all_found = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));
    int delimiter = options.zero_terminated ? '\0' : '\n';

    if (operand_count == 0) {
        if (!bx_printenv_print_all(&writer, options.zero_terminated, &diag)) {
            return diag.exit_status;
        }
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            const char* value = getenv(argv[i]);
            if (value == NULL) {
                all_found = false;
                continue;
            }

            if (!bx_printenv_emit_delimited(&writer, value, delimiter, &diag)) {
                return diag.exit_status;
            }
        }
    }

    if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_printenv_write_error(&diag);
        return diag.exit_status;
    }

    return all_found ? 0 : 1;
}
