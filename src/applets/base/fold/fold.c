#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

enum {
    BX_FOLD_OPT_HELP = 256,
    BX_FOLD_OPT_VERSION,
};

struct bx_fold_options {
    size_t width;
    bool bytes;
    bool spaces;
    bool show_help;
    bool show_version;
};

static void bx_fold_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Wrap input lines in each FILE, writing to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --bytes         count bytes rather than columns\n");
    fprintf(stream, "  -s, --spaces        break at spaces\n");
    fprintf(stream, "  -w, --width=WIDTH   use WIDTH columns instead of 80\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_fold_parse_options(int argc, char** argv, struct bx_fold_options* options,
                                  const char** paths, size_t* count, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'b', 'b', "bytes", BX_OPTION_NO_VALUE},
        {'s', 's', "spaces", BX_OPTION_NO_VALUE},
        {'w', 'w', "width", BX_OPTION_REQUIRED_VALUE},
        {BX_FOLD_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_FOLD_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            paths[(*count)++] = option.token;
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
            return false;
        }

        switch (option.spec->id) {
            case 'b':
                options->bytes = true;
                break;
            case 's':
                options->spaces = true;
                break;
            case 'w':
                if (!bx_args_parse_size_range(option.value, 1u, SIZE_MAX, &options->width)) {
                    bx_diag(diag, "invalid width: '%s'", option.value);
                    return false;
                }
                break;
            case BX_FOLD_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_FOLD_OPT_VERSION:
                options->show_version = true;
                return true;
        }
    }

    return true;
}

static bool bx_fold_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_fold_write(
    struct bx_line_writer* writer,
    const void* data,
    size_t length,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_write(writer, data, length)) {
        return bx_fold_write_error(diag);
    }
    return true;
}

static bool bx_fold_putc(
    struct bx_line_writer* writer,
    char ch,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_putc(writer, ch)) {
        return bx_fold_write_error(diag);
    }
    return true;
}

static bool fold_file(
    FILE* f,
    struct bx_fold_options* options,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;

    while ((len = getline(&line, &line_cap, f)) != -1) {
        size_t col = 0;
        size_t last_space_pos = 0;
        size_t start = 0;

        for (size_t i = 0; i < (size_t)len; i++) {
            int c = (unsigned char)line[i];

            if (c == '\n') {
                if (!bx_fold_write(writer, &line[start], i - start + 1, diag)) {
                    free(line);
                    return false;
                }
                start = i + 1;
                col = 0;
                last_space_pos = 0;
                continue;
            }

            size_t next_col = col;
            if (options->bytes) {
                next_col++;
            }
            else {
                if (c == '\t')
                    next_col = (col / 8 + 1) * 8;
                else if (c == '\b')
                    next_col = (col > 0) ? col - 1 : 0;
                else if (c == '\r')
                    next_col = 0;
                else
                    next_col++;
            }

            if (next_col > options->width && col > 0) {
                // Time to wrap
                size_t wrap_pos = i;
                if (options->spaces && last_space_pos > start) {
                    wrap_pos = last_space_pos;
                    if (!bx_fold_write(writer, &line[start], wrap_pos - start, diag) ||
                        !bx_fold_putc(writer, '\n', diag)) {
                        free(line);
                        return false;
                    }
                    start = wrap_pos;
                    // Skip the space that caused the break
                    if (line[start] == ' ') {
                        start++;
                    }
                    // Re-calculate column for the new line starting from start to i
                    col = 0;
                    for (size_t j = start; j <= i; j++) {
                        int c2 = (unsigned char)line[j];
                        if (options->bytes)
                            col++;
                        else {
                            if (c2 == '\t')
                                col = (col / 8 + 1) * 8;
                            else if (c2 == '\b')
                                col = (col > 0) ? col - 1 : 0;
                            else if (c2 == '\r')
                                col = 0;
                            else
                                col++;
                        }
                    }
                    last_space_pos = 0;
                }
                else {
                    if (!bx_fold_write(writer, &line[start], i - start, diag) ||
                        !bx_fold_putc(writer, '\n', diag)) {
                        free(line);
                        return false;
                    }
                    start = i;
                    col = next_col - col;  // Column of char c on new line
                    if (c == '\t' && !options->bytes)
                        col = 8;  // Reset tab on new line
                    // Wait, if it was a tab, col becomes 8.
                    // Actually, simpler to just re-scanchar c
                    col = 0;
                    if (options->bytes)
                        col = 1;
                    else {
                        if (c == '\t')
                            col = 8;
                        else if (c == '\b')
                            col = 0;
                        else if (c == '\r')
                            col = 0;
                        else
                            col = 1;
                    }
                    last_space_pos = 0;
                }
            }
            else {
                col = next_col;
                if (isspace(c) && c != '\r' && c != '\t' && c != '\b') {
                    last_space_pos = i + 1;
                }
            }
        }
        if (start < (size_t)len) {
            if (!bx_fold_write(writer, &line[start], (size_t)len - start, diag)) {
                free(line);
                return false;
            }
        }
    }
    free(line);
    return true;
}

int bx_fold_main(int argc, char** argv) {
    struct bx_fold_options options = {.width = 80};
    struct bx_diag_ctx diag = {.progname = "fold"};
    const char** paths = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*paths)) : NULL;
    if (argc > 1 && paths == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t count = 0;

    if (!bx_fold_parse_options(argc, argv, &options, paths, &count, &diag))
        goto done;
    if (options.show_help) {
        bx_fold_print_help(stdout, diag.progname);
        goto done;
    }
    if (options.show_version) {
        bx_cli_print_version(diag.progname);
        goto done;
    }

    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    size_t files = count == 0 ? 1u : count;
    for (size_t i = 0; ok && i < files; i++) {
        const char* filename = count == 0 ? "-" : paths[i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }
        ok = fold_file(f, &options, &writer, &diag);
        bx_fclose_nonstdio(f, is_stdio);
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_fold_write_error(&diag);
    }

done:
    free(paths);
    return diag.exit_status;
}
