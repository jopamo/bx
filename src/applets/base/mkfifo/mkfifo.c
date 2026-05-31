#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/mode_parse.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/args_common.h"

struct bx_mkfifo_options {
    const char* progname;
    bool mode_set;
    mode_t mode;
    bool show_help;
    bool show_version;
};

static void bx_mkfifo_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME...\n", progname);
    fprintf(stream, "Create the FIFO special files NAMEs.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -m, --mode=MODE  set file permission bits to MODE, not a=rw - umask\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_mkfifo_parse_mode(const char* text, mode_t* mode_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "invalid mode '%s'", (text != NULL) ? text : "");
        return false;
    }

    struct bx_mode_parse_params params = {
        .initial_mode = 0666u,
        .result_mask = 0777u,
        .max_numeric_mode = 0777u,
        .umask_value = bx_mode_current_umask(),
        .sticky_bit = 0u,
        .x_policy = BX_MODE_X_IF_ANY_EXEC,
        .is_directory = false,
        .apply_umask_when_who_omitted = true,
        .allow_setuid = false,
        .allow_setgid = false,
        .allow_sticky = false,
    };

    if (bx_mode_parse(text, &params, mode_out)) {
        return true;
    }

    bx_diag(diag, "invalid mode '%s'", text);
    return false;
}

static bool bx_mkfifo_parse_options(int argc, char** argv, struct bx_mkfifo_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"mode", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "mkfifo");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+:m:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'm':
                if (!bx_mkfifo_parse_mode(optarg, &options->mode, diag)) {
                    return false;
                }
                options->mode_set = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
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

int bx_mkfifo_main(int argc, char** argv) {
    struct bx_mkfifo_options options;
    struct bx_diag_ctx diag = {
        .progname = "mkfifo",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mkfifo_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mkfifo_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_cli_diag_missing_operand(&diag);
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        mode_t create_mode = options.mode_set ? options.mode : 0666u;
        if (bx_fd_mkfifoat(AT_FDCWD, argv[i], create_mode) != 0) {
            bx_perror_path(&diag, argv[i]);
        }
        else if (options.mode_set && chmod(argv[i], options.mode) != 0) {
            bx_perror_path(&diag, argv[i]);
        }
    }

    return diag.exit_status;
}
