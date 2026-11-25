#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

enum {
    BX_PASTE_OPT_HELP = 256,
    BX_PASTE_OPT_VERSION,
};

struct bx_paste_options {
    int* delimiters;
    size_t delimiters_len;
    const char* delimiter_spec;
    bool serial;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_paste_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Write lines consisting of the sequentially corresponding lines from\n");
    fprintf(stream, "each FILE, separated by TABs, to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --delimiters=LIST   reuse characters from LIST instead of TABs;\n");
    fprintf(stream, "                            backslash escapes are supported\n");
    fprintf(stream, "  -s, --serial            paste one file at a time instead of in parallel\n");
    fprintf(stream, "  -z, --zero-terminated   line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_paste_parse_delimiters(const char* spec, struct bx_paste_options* options, struct bx_diag_ctx* diag) {
    size_t spec_len = strlen(spec);
    size_t capacity = spec_len == 0 ? 1 : spec_len;
    int* delimiters = xmalloc(capacity * sizeof(*delimiters));
    size_t count = 0;

    if (spec_len == 0) {
        delimiters[count++] = -1;
    }

    for (size_t i = 0; i < spec_len; i++) {
        unsigned char ch = (unsigned char)spec[i];
        if (ch != '\\') {
            delimiters[count++] = (int)ch;
            continue;
        }

        if (i + 1 >= spec_len) {
            free(delimiters);
            bx_diag(diag, "delimiter list ends with an unescaped backslash: %s", spec);
            return false;
        }

        i++;
        switch (spec[i]) {
            case '0':
                delimiters[count++] = -1;
                break;
            case 'b':
                delimiters[count++] = '\b';
                break;
            case 'f':
                delimiters[count++] = '\f';
                break;
            case 'n':
                delimiters[count++] = '\n';
                break;
            case 'r':
                delimiters[count++] = '\r';
                break;
            case 't':
                delimiters[count++] = '\t';
                break;
            case 'v':
                delimiters[count++] = '\v';
                break;
            case '\\':
                delimiters[count++] = '\\';
                break;
            default:
                delimiters[count++] = (unsigned char)spec[i];
                break;
        }
    }

    options->delimiters = delimiters;
    options->delimiters_len = count;
    return true;
}

static bool bx_paste_parse_options(
    int argc,
    char** argv,
    struct bx_paste_options* options,
    const char** filenames,
    size_t* count,
    struct bx_diag_ctx* diag
) {
    static const struct bx_option_spec specs[] = {
        {'d', 'd', "delimiters", BX_OPTION_REQUIRED_VALUE},
        {'s', 's', "serial", BX_OPTION_NO_VALUE},
        {'z', 'z', "zero-terminated", BX_OPTION_NO_VALUE},
        {BX_PASTE_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {BX_PASTE_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            filenames[(*count)++] = option.token;
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
            bx_cli_print_try_help(diag->progname);
            return false;
        }

        switch (option.spec->id) {
            case 'd':
                options->delimiter_spec = option.value;
                break;
            case 's':
                options->serial = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case BX_PASTE_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_PASTE_OPT_VERSION:
                options->show_version = true;
                return true;
        }
    }

    return bx_paste_parse_delimiters(options->delimiter_spec, options, diag);
}

static bool bx_paste_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_paste_emit_delimiter(
    struct bx_line_writer* writer,
    const struct bx_paste_options* options,
    size_t index,
    struct bx_diag_ctx* diag
) {
    int delim = options->delimiters[index % options->delimiters_len];
    if (delim >= 0) {
        if (!bx_line_writer_putc(writer, (char)delim)) {
            return bx_paste_write_error(diag);
        }
    }
    return true;
}

static bool paste_serial(
    size_t num_files,
    const char* const* filenames,
    struct bx_paste_options* options,
    struct bx_diag_ctx* diag,
    struct bx_line_writer* writer
) {
    int delimiter = options->zero_terminated ? '\0' : '\n';
    bool ok = true;

    for (size_t i = 0; i < num_files; i++) {
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filenames[i], "r", &is_stdio);
        if (!f) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
            continue;
        }

        char* line = NULL;
        size_t cap = 0;
        ssize_t len;
        bool first_line = true;
        size_t delim_idx = 0;
        while ((len = getdelim(&line, &cap, delimiter, f)) != -1) {
            if (!first_line) {
                if (!bx_paste_emit_delimiter(writer, options, delim_idx, diag)) {
                    ok = false;
                    break;
                }
                delim_idx++;
            }
            if (line[len - 1] == delimiter)
                line[len - 1] = '\0';
            if (!bx_line_writer_puts(writer, line)) {
                ok = bx_paste_write_error(diag);
                break;
            }
            first_line = false;
        }
        if (ok && !bx_line_writer_putc(writer, (char)delimiter)) {
            ok = bx_paste_write_error(diag);
        }
        free(line);
        bx_fclose_nonstdio(f, is_stdio);
        if (!ok) {
            break;
        }
    }

    return ok;
}

static bool paste_parallel(
    size_t num_files,
    const char* const* filenames,
    struct bx_paste_options* options,
    struct bx_diag_ctx* diag,
    struct bx_line_writer* writer
) {
    FILE** files = xmalloc(num_files * sizeof(FILE*));
    bool* is_stdio = xmalloc(num_files * sizeof(bool));
    bool open_failed = false;
    bool ok = true;
    for (size_t i = 0; i < num_files; i++) {
        files[i] = bx_fopen_dash(filenames[i], "r", &is_stdio[i]);
        if (!files[i]) {
            bx_diag(diag, "%s: %s", filenames[i], strerror(errno));
            open_failed = true;
        }
    }

    if (open_failed) {
        for (size_t i = 0; i < num_files; i++) {
            bx_fclose_nonstdio(files[i], is_stdio[i]);
        }
        free(files);
        free(is_stdio);
        return true;
    }

    int delimiter = options->zero_terminated ? '\0' : '\n';

    char* line = NULL;
    size_t line_cap = 0;
    char** row_fields = xmalloc(num_files * sizeof(char*));

    while (true) {
        bool any_active = false;
        for (size_t i = 0; i < num_files; i++) {
            row_fields[i] = NULL;
            if (files[i]) {
                ssize_t len = getdelim(&line, &line_cap, delimiter, files[i]);
                if (len != -1) {
                    any_active = true;
                    if (line[len - 1] == delimiter)
                        line[len - 1] = '\0';
                    row_fields[i] = xstrdup(line);
                }
                else {
                    bx_fclose_nonstdio(files[i], is_stdio[i]);
                    files[i] = NULL;
                }
            }
        }

        if (!any_active)
            break;

        for (size_t i = 0; i < num_files; i++) {
            if (ok && row_fields[i] && !bx_line_writer_puts(writer, row_fields[i])) {
                ok = bx_paste_write_error(diag);
            }
            if (i + 1 < num_files) {
                if (ok && !bx_paste_emit_delimiter(writer, options, i, diag)) {
                    ok = false;
                }
            }
            free(row_fields[i]);
        }
        if (ok && !bx_line_writer_putc(writer, (char)delimiter)) {
            ok = bx_paste_write_error(diag);
        }
        if (!ok) {
            break;
        }
    }

    for (size_t i = 0; i < num_files; i++)
        bx_fclose_nonstdio(files[i], is_stdio[i]);
    free(files);
    free(is_stdio);
    free(line);
    free(row_fields);
    return ok;
}

int bx_paste_main(int argc, char** argv) {
    struct bx_paste_options options = {.delimiter_spec = "\t"};
    struct bx_diag_ctx diag = {
        .progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "paste"),
    };
    const char** operands = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*operands)) : NULL;
    if (argc > 1 && operands == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    size_t count = 0;
    if (!bx_paste_parse_options(argc, argv, &options, operands, &count, &diag))
        goto done;
    if (options.show_help) {
        bx_paste_print_help(stdout, diag.progname);
        goto done;
    }
    if (options.show_version) {
        bx_cli_print_version(diag.progname);
        goto done;
    }

    const char* default_filenames[] = {"-"};
    const char* const* filenames = count == 0 ? default_filenames : operands;
    size_t files = count == 0 ? 1u : count;

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    bool ok = options.serial
        ? paste_serial(files, filenames, &options, &diag, &writer)
        : paste_parallel(files, filenames, &options, &diag, &writer);

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_paste_write_error(&diag);
    }

done:
    free(options.delimiters);
    free(operands);
    return diag.exit_status;
}
