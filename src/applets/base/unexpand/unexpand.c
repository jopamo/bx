#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/size_parse.h"

enum {
    BX_UNEXPAND_OPT_FIRST_ONLY = 256,
    BX_UNEXPAND_OPT_HELP,
    BX_UNEXPAND_OPT_VERSION,
};

struct bx_unexpand_options {
    const char* progname;
    size_t tab_size;
    bool all;
    bool first_only;
    bool tabs_specified;
    bool show_help;
    bool show_version;
};

static void bx_unexpand_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Convert blanks in each FILE to tabs, writing to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all                convert all blanks, instead of just initial blanks\n");
    fprintf(stream, "      --first-only         convert only leading sequences of blanks (overrides -a)\n");
    fprintf(stream, "  -t, --tabs=N             have tabs N characters apart instead of 8 (enables -a)\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

static bool bx_unexpand_parse_tab_size(const char* text, struct bx_unexpand_options* options, struct bx_diag_ctx* diag) {
    uintmax_t parsed = 0;

    if (text != NULL && text[0] == '\0') {
        options->tab_size = 8;
        return true;
    }

    if (!bx_size_parse_uint(text, &parsed)) {
        bx_diag(diag, "tab size contains invalid character(s): '%s'", text == NULL ? "" : text);
        return false;
    }
    if (parsed == 0) {
        bx_diag(diag, "tab size cannot be 0");
        return false;
    }
    if (parsed > (uintmax_t)SIZE_MAX) {
        bx_diag(diag, "tab size too large");
        return false;
    }

    options->tab_size = (size_t)parsed;
    return true;
}

static void bx_unexpand_diag_option_requires_arg(struct bx_diag_ctx* diag, int missing_optopt, int missing_optind, int argc, char* const argv[]) {
    if (missing_optind > 0 && missing_optind <= argc && argv[missing_optind - 1] != NULL && strncmp(argv[missing_optind - 1], "--", 2) == 0) {
        bx_diag(diag, "option '%s' requires an argument", argv[missing_optind - 1]);
        return;
    }

    bx_cli_diag_option_requires_arg(diag, missing_optopt, missing_optind, argc, argv);
}

static bool bx_unexpand_parse_options(int argc, char** argv, struct bx_unexpand_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"first-only", no_argument, NULL, BX_UNEXPAND_OPT_FIRST_ONLY},
        {"tabs", required_argument, NULL, 't'},
        {"help", no_argument, NULL, BX_UNEXPAND_OPT_HELP},
        {"version", no_argument, NULL, BX_UNEXPAND_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "unexpand");
    options->tab_size = 8;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, ":at:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->all = true;
                break;
            case BX_UNEXPAND_OPT_FIRST_ONLY:
                options->first_only = true;
                break;
            case 't':
                options->tabs_specified = true;
                if (!bx_unexpand_parse_tab_size(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_UNEXPAND_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_UNEXPAND_OPT_VERSION:
                options->show_version = true;
                return true;
            case ':':
                bx_unexpand_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(options->progname);
                return false;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(options->progname);
                return false;
            default:
                return false;
        }
    }

    if (options->first_only) {
        options->all = false;
    }
    else if (options->tabs_specified) {
        options->all = true;
    }

    *first_operand = optind;
    return true;
}

static bool bx_unexpand_putc(int c, struct bx_diag_ctx* diag) {
    if (putchar(c) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static size_t bx_unexpand_next_tab_stop(size_t column, size_t tab_size) {
    size_t advance = tab_size - (column % tab_size);
    if (advance > SIZE_MAX - column) {
        return SIZE_MAX;
    }
    return column + advance;
}

static bool bx_unexpand_flush_spaces(size_t start_column, size_t count, const struct bx_unexpand_options* options, struct bx_diag_ctx* diag) {
    size_t end_column = 0;
    size_t column = start_column;

    if (count == 0) {
        return true;
    }

    if (count > SIZE_MAX - start_column) {
        while (count-- > 0) {
            if (!bx_unexpand_putc(' ', diag)) {
                return false;
            }
        }
        return true;
    }

    end_column = start_column + count;
    if (count > 1) {
        while (column < end_column) {
            size_t next_stop = bx_unexpand_next_tab_stop(column, options->tab_size);
            if (next_stop <= column || next_stop > end_column) {
                break;
            }
            if (!bx_unexpand_putc('\t', diag)) {
                return false;
            }
            column = next_stop;
        }
    }

    while (column < end_column) {
        if (!bx_unexpand_putc(' ', diag)) {
            return false;
        }
        column++;
    }

    return true;
}

static bool unexpand_file(FILE* f, const struct bx_unexpand_options* options, struct bx_diag_ctx* diag) {
    int c;
    size_t column = 0;
    size_t pending_start_column = 0;
    size_t pending_spaces = 0;
    bool initial_blanks = true;

    while ((c = getc(f)) != EOF) {
        if (c == ' ' && (options->all || initial_blanks)) {
            if (pending_spaces == 0) {
                pending_start_column = column;
            }
            pending_spaces++;
            column++;
        }
        else {
            if (!bx_unexpand_flush_spaces(pending_start_column, pending_spaces, options, diag)) {
                return false;
            }
            pending_spaces = 0;
            if (!bx_unexpand_putc(c, diag)) {
                return false;
            }
            if (c == '\n') {
                column = 0;
                initial_blanks = true;
            }
            else if (c == '\t') {
                column = bx_unexpand_next_tab_stop(column, options->tab_size);
            }
            else if (c == '\b') {
                if (column > 0) {
                    column--;
                }
            }
            else if (c == '\r') {
                column = 0;
            }
            else {
                column++;
                if (c != ' ' && c != '\t') {
                    initial_blanks = false;
                }
            }
        }
    }

    if (ferror(f)) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }

    return bx_unexpand_flush_spaces(pending_start_column, pending_spaces, options, diag);
}

int bx_unexpand_main(int argc, char** argv) {
    struct bx_unexpand_options options;
    struct bx_diag_ctx diag = {
        .progname = "unexpand",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_unexpand_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status == 0 ? 1 : diag.exit_status;
    }
    if (options.show_help) {
        bx_unexpand_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (first_operand == argc) {
        if (!unexpand_file(stdin, &options, &diag)) {
            return diag.exit_status == 0 ? 1 : diag.exit_status;
        }
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            bool is_stdio = false;
            FILE* f = bx_fopen_dash(argv[i], "r", &is_stdio);
            if (!f) {
                bx_perror_path(&diag, argv[i]);
                continue;
            }
            if (!unexpand_file(f, &options, &diag)) {
                bx_fclose_nonstdio(f, is_stdio);
                break;
            }
            bx_fclose_nonstdio(f, is_stdio);
        }
    }

    if (!bx_cli_flush_stdout(&diag)) {
        return diag.exit_status == 0 ? 1 : diag.exit_status;
    }

    return diag.exit_status;
}
