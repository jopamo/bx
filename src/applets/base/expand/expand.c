#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/size_parse.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"

struct bx_expand_options {
    const char* progname;
    size_t* tab_stops;
    size_t num_tab_stops;
    size_t repeat_size;
    bool initial_only;
    bool show_help;
    bool show_version;
};

static void bx_expand_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Convert tabs in each FILE to spaces, writing to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -i, --initial       do not convert tabs after non-blanks\n");
    fprintf(stream, "  -t, --tabs=N        have tabs N characters apart, not 8\n");
    fprintf(stream, "  -t, --tabs=LIST     use comma-separated list of tab positions.\n");
    fprintf(stream, "                        The last specified position can be prefixed with '/'\n");
    fprintf(stream, "                        to specify a repeat size after the last explicitly\n");
    fprintf(stream, "                        specified tab stop.  Or '+' to specify relative to previous.\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool parse_tabs(const char* list, struct bx_expand_options* options, struct bx_diag_ctx* diag) {
    char* copy = xstrdup(list);
    size_t count = 1;
    for (const char* c = list; *c; c++)
        if (*c == ',')
            count++;

    options->tab_stops = xmalloc(count * sizeof(size_t));
    options->num_tab_stops = 0;
    options->repeat_size = 0;

    char* tok_start = copy;
    while (tok_start) {
        char* tok_end = strchr(tok_start, ',');
        if (tok_end)
            *tok_end = '\0';

        char* s = tok_start;
        bool relative = false;
        bool repeat = false;
        if (*s == '+') {
            relative = true;
            s++;
        }
        else if (*s == '/') {
            repeat = true;
            s++;
        }

        uintmax_t parsed = 0;
        if (!((repeat && s[0] == '\0') || bx_size_parse_uint(s, &parsed)) ||
            parsed > (uintmax_t)SIZE_MAX || (parsed == 0 && !repeat)) {
            bx_diag(diag, "tab size contains invalid character(s): '%s'", tok_start);
            free(copy);
            return false;
        }
        size_t val = (size_t)parsed;

        if (tok_end == NULL && (relative || repeat)) {
            options->repeat_size = val;
        }
        else {
            size_t pos = val;
            if (relative && options->num_tab_stops > 0) {
                size_t previous = options->tab_stops[options->num_tab_stops - 1];
                if (val > (uintmax_t)(SIZE_MAX - previous)) {
                    bx_diag(diag, "tab offset is not increasing");
                    free(copy);
                    return false;
                }
                pos = previous + val;
            }
            if (options->num_tab_stops > 0 && pos <= options->tab_stops[options->num_tab_stops - 1]) {
                bx_diag(diag, "tab offset is not increasing");
                free(copy);
                return false;
            }
            options->tab_stops[options->num_tab_stops++] = pos;
        }

        tok_start = tok_end ? tok_end + 1 : NULL;
    }

    if (options->num_tab_stops == 1 && options->repeat_size == 0) {
        options->repeat_size = options->tab_stops[0];
        options->num_tab_stops = 0;
    }

    free(copy);
    return true;
}

static bool bx_expand_parse_options(int argc, char** argv, struct bx_expand_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"initial", no_argument, NULL, 'i'}, {"tabs", required_argument, NULL, 't'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "expand";
    diag->progname = options->progname;

    bx_args_getopt_reset();

    const char* tabs_str = NULL;

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "it:", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'i':
                options->initial_only = true;
                break;
            case 't':
                tabs_str = optarg;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    if (tabs_str) {
        if (!parse_tabs(tabs_str, options, diag))
            return false;
    }
    else {
        options->repeat_size = 8;
    }

    *first_operand = optind;
    return true;
}

static bool bx_expand_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_expand_write_char(
    struct bx_line_writer* writer,
    char ch,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_putc(writer, ch)) {
        return bx_expand_write_error(diag);
    }
    return true;
}

static bool expand_file(
    FILE* f,
    struct bx_expand_options* options,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
    int c;
    size_t col = 0;
    bool initial = true;

    while ((c = getc(f)) != EOF) {
        if (c == '\t' && (!options->initial_only || initial)) {
            size_t next_stop = 0;
            bool found = false;
            for (size_t i = 0; i < options->num_tab_stops; i++) {
                if (options->tab_stops[i] > col) {
                    next_stop = options->tab_stops[i];
                    found = true;
                    break;
                }
            }

            if (!found) {
                if (options->repeat_size > 0) {
                    size_t base = options->num_tab_stops > 0 ? options->tab_stops[options->num_tab_stops - 1] : 0;
                    if (col < base)
                        base = 0;  // Should not happen with increasing stops
                    next_stop = base + ((col - base) / options->repeat_size + 1) * options->repeat_size;
                }
                else {
                    next_stop = col + 1;
                }
            }

            while (col < next_stop) {
                if (!bx_expand_write_char(writer, ' ', diag)) {
                    return false;
                }
                col++;
            }
        }
        else {
            if (!bx_expand_write_char(writer, (char)c, diag)) {
                return false;
            }
            if (c == '\n') {
                col = 0;
                initial = true;
            }
            else if (c == '\b') {
                if (col > 0)
                    col--;
            }
            else if (c == '\r') {
                col = 0;
            }
            else {
                col++;  // Basic column counting (doesn't handle multi-byte/wide chars yet)
                if (!isspace(c))
                    initial = false;
            }
        }
    }

    return true;
}

int bx_expand_main(int argc, char** argv) {
    struct bx_expand_options options;
    struct bx_diag_ctx diag = {.progname = "expand", .exit_status = 0};
    int first_operand = 0;

    if (!bx_expand_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_expand_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    for (int i = 0; ok && (i < num_files || (i == 0 && num_files == 0)); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }
        ok = expand_file(f, &options, &writer, &diag);
        bx_fclose_nonstdio(f, is_stdio);
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_expand_write_error(&diag);
    }

    free(options.tab_stops);
    return diag.exit_status;
}
