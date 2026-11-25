#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/size_parse.h"
#include "lib/option_scan.h"
#include "lib/line_writer.h"

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
    const char** files;
    size_t file_count;
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

static bool bx_unexpand_parse_options(int argc, char** argv, struct bx_unexpand_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'a', 'a', "all", BX_OPTION_NO_VALUE},
        {BX_UNEXPAND_OPT_FIRST_ONLY, 0, "first-only", BX_OPTION_NO_VALUE},
        {'t', 't', "tabs", BX_OPTION_REQUIRED_VALUE},
        {BX_UNEXPAND_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_UNEXPAND_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "unexpand");
    options->tab_size = 8;
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!options->files) {
                options->files = calloc((size_t)argc - option.index, sizeof(*options->files));
                if (!options->files) {
                    bx_perror_path(diag, "allocate input operands");
                    return false;
                }
            }
            options->files[options->file_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(options->progname);
            return false;
        }
        switch (option.spec->id) {
            case 'a':
                options->all = true;
                break;
            case BX_UNEXPAND_OPT_FIRST_ONLY:
                options->first_only = true;
                break;
            case 't':
                options->tabs_specified = true;
                if (!bx_unexpand_parse_tab_size(option.value, options, diag)) {
                    return false;
                }
                break;
            case BX_UNEXPAND_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_UNEXPAND_OPT_VERSION:
                options->show_version = true;
                return true;
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

    return true;
}

static bool bx_unexpand_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_unexpand_putc(struct bx_line_writer* writer, int c, struct bx_diag_ctx* diag) {
    if (!bx_line_writer_putc(writer, (char)c)) {
        return bx_unexpand_write_error(diag);
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

static bool bx_unexpand_flush_spaces(
    struct bx_line_writer* writer,
    size_t start_column,
    size_t count,
    const struct bx_unexpand_options* options,
    struct bx_diag_ctx* diag
) {
    size_t end_column = 0;
    size_t column = start_column;

    if (count == 0) {
        return true;
    }

    if (count > SIZE_MAX - start_column) {
        while (count-- > 0) {
            if (!bx_unexpand_putc(writer, ' ', diag)) {
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
            if (!bx_unexpand_putc(writer, '\t', diag)) {
                return false;
            }
            column = next_stop;
        }
    }

    while (column < end_column) {
        if (!bx_unexpand_putc(writer, ' ', diag)) {
            return false;
        }
        column++;
    }

    return true;
}

static bool unexpand_file(
    FILE* f,
    const struct bx_unexpand_options* options,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
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
            if (!bx_unexpand_flush_spaces(writer, pending_start_column, pending_spaces, options, diag)) {
                return false;
            }
            pending_spaces = 0;
            if (!bx_unexpand_putc(writer, c, diag)) {
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

    return bx_unexpand_flush_spaces(writer, pending_start_column, pending_spaces, options, diag);
}

int bx_unexpand_main(int argc, char** argv) {
    struct bx_unexpand_options options;
    struct bx_diag_ctx diag = {
        .progname = "unexpand",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_unexpand_parse_options(argc, argv, &options, &diag)) {
        if (diag.exit_status == 0)
            diag.exit_status = 1;
        goto done;
    }
    if (options.show_help) {
        bx_unexpand_print_help(stdout, options.progname);
        goto done;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        goto done;
    }

    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    if (!options.file_count) {
        ok = unexpand_file(stdin, &options, &writer, &diag);
    }
    else {
        for (size_t i = 0; ok && i < options.file_count; i++) {
            bool is_stdio = false;
            FILE* f = bx_fopen_dash(options.files[i], "r", &is_stdio);
            if (!f) {
                bx_perror_path(&diag, options.files[i]);
                continue;
            }
            ok = unexpand_file(f, &options, &writer, &diag);
            bx_fclose_nonstdio(f, is_stdio);
        }
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_unexpand_write_error(&diag);
    }

done:
    free(options.files);
    return diag.exit_status;
}
