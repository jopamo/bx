#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

enum numbering_style { STYLE_ALL, STYLE_NONEMPTY, STYLE_NONE, STYLE_REGEX };

struct section_style {
    enum numbering_style style;
    regex_t regex;
};

struct bx_nl_options {
    const char* progname;
    struct section_style body;
    struct section_style header;
    struct section_style footer;
    char section_delim[2];
    int line_increment;
    int join_blank_lines;
    const char* number_format;
    bool no_renumber;
    const char* number_separator;
    long long starting_line_number;
    int number_width;
    bool show_help;
    bool show_version;
    const char** files;
    size_t file_count;
};

static void bx_nl_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Write each FILE to standard output, with line numbers added.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --body-numbering=STYLE      use STYLE for numbering body lines\n");
    fprintf(stream, "  -d, --section-delimiter=CC      use CC for separating sections\n");
    fprintf(stream, "  -f, --footer-numbering=STYLE    use STYLE for numbering footer lines\n");
    fprintf(stream, "  -h, --header-numbering=STYLE    use STYLE for numbering header lines\n");
    fprintf(stream, "  -i, --line-increment=NUMBER     line number increment at each line\n");
    fprintf(stream, "  -l, --join-blank-lines=NUMBER   group of NUMBER empty lines counted as one\n");
    fprintf(stream, "  -n, --number-format=FORMAT      insert line numbers according to FORMAT\n");
    fprintf(stream, "  -p, --no-renumber               do not reset line numbers at each section\n");
    fprintf(stream, "  -s, --number-separator=STRING   add STRING after line number\n");
    fprintf(stream, "  -v, --starting-line-number=NUMBER  first line number on each section\n");
    fprintf(stream, "  -w, --number-width=NUMBER       use NUMBER columns for line numbers\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "STYLE is one of:\n");
    fprintf(stream, "  a       number all lines\n");
    fprintf(stream, "  t       number only nonempty lines\n");
    fprintf(stream, "  n       do not number lines\n");
    fprintf(stream, "  pBRE    number only lines that contain a match for the basic regular\n");
    fprintf(stream, "            expression, BRE\n");
    fprintf(stream, "\n");
    fprintf(stream, "FORMAT is one of:\n");
    fprintf(stream, "  ln      left justified, no leading zeros\n");
    fprintf(stream, "  rn      right justified, no leading zeros\n");
    fprintf(stream, "  rz      right justified, leading zeros\n");
}

static void clear_style(struct section_style* style) {
    if (style->style == STYLE_REGEX)
        regfree(&style->regex);
    style->style = STYLE_NONE;
}

static bool parse_style(const char* str, struct section_style* style, struct bx_diag_ctx* diag) {
    clear_style(style);
    if (strcmp(str, "a") == 0)
        style->style = STYLE_ALL;
    else if (strcmp(str, "t") == 0)
        style->style = STYLE_NONEMPTY;
    else if (strcmp(str, "n") == 0)
        style->style = STYLE_NONE;
    else if (str[0] == 'p') {
        if (regcomp(&style->regex, str + 1, REG_NOSUB) != 0) {
            bx_diag(diag, "invalid regular expression: %s", str + 1);
            return false;
        }
        style->style = STYLE_REGEX;
    }
    else {
        bx_diag(diag, "invalid body numbering style: '%s'", str);
        return false;
    }
    return true;
}

static bool bx_nl_parse_options(int argc, char** argv, struct bx_nl_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'b', 'b', "body-numbering", BX_OPTION_REQUIRED_VALUE},
        {'d', 'd', "section-delimiter", BX_OPTION_REQUIRED_VALUE},
        {'f', 'f', "footer-numbering", BX_OPTION_REQUIRED_VALUE},
        {'h', 'h', "header-numbering", BX_OPTION_REQUIRED_VALUE},
        {'i', 'i', "line-increment", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', "join-blank-lines", BX_OPTION_REQUIRED_VALUE},
        {'n', 'n', "number-format", BX_OPTION_REQUIRED_VALUE},
        {'p', 'p', "no-renumber", BX_OPTION_NO_VALUE},
        {'s', 's', "number-separator", BX_OPTION_REQUIRED_VALUE},
        {'v', 'v', "starting-line-number", BX_OPTION_REQUIRED_VALUE},
        {'w', 'w', "number-width", BX_OPTION_REQUIRED_VALUE},
        {1, 0, "help", BX_OPTION_NO_VALUE},
        {2, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "nl";
    options->body.style = STYLE_NONEMPTY;
    options->header.style = STYLE_NONE;
    options->footer.style = STYLE_NONE;
    options->section_delim[0] = '\\';
    options->section_delim[1] = ':';
    options->line_increment = 1;
    options->join_blank_lines = 1;
    options->number_format = "rn";
    options->number_separator = "\t";
    options->starting_line_number = 1;
    options->number_width = 6;
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
            if (option.event == BX_OPTION_MISSING_VALUE && option.spec)
                bx_diag(diag, "option requires an argument -- '%c'", option.spec->short_name);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        const char* value = option.value;
        switch (option.spec->id) {
            case 'b':
                if (!parse_style(value, &options->body, diag))
                    return false;
                break;
            case 'd':
                if (strlen(value) == 1) {
                    options->section_delim[0] = value[0];
                    options->section_delim[1] = ':';
                }
                else if (strlen(value) == 2) {
                    options->section_delim[0] = value[0];
                    options->section_delim[1] = value[1];
                }
                else {
                    bx_diag(diag, "invalid section delimiter: '%s'", value);
                    return false;
                }
                break;
            case 'f':
                if (!parse_style(value, &options->footer, diag))
                    return false;
                break;
            case 'h':
                if (!parse_style(value, &options->header, diag))
                    return false;
                break;
            case 'i':
                if (!bx_args_parse_int_range(value, INT_MIN, INT_MAX, &options->line_increment)) {
                    bx_diag(diag, "invalid line increment: '%s'", value);
                    return false;
                }
                break;
            case 'l':
                if (!bx_args_parse_int_range(value, INT_MIN, INT_MAX, &options->join_blank_lines)) {
                    bx_diag(diag, "invalid blank line count: '%s'", value);
                    return false;
                }
                break;
            case 'n':
                if (strcmp(value, "ln") != 0 && strcmp(value, "rn") != 0 && strcmp(value, "rz") != 0) {
                    bx_diag(diag, "invalid number format: '%s'", value);
                    return false;
                }
                options->number_format = value;
                break;
            case 'p':
                options->no_renumber = true;
                break;
            case 's':
                options->number_separator = value;
                break;
            case 'v':
                if (!bx_args_parse_llong_range(value, LLONG_MIN, LLONG_MAX, &options->starting_line_number)) {
                    bx_diag(diag, "invalid starting line number: '%s'", value);
                    return false;
                }
                break;
            case 'w':
                if (!bx_args_parse_int_range(value, INT_MIN, INT_MAX, &options->number_width)) {
                    bx_diag(diag, "invalid number width: '%s'", value);
                    return false;
                }
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    return true;
}

static bool bx_nl_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_nl_write_padding(struct bx_line_writer* writer, size_t count, struct bx_diag_ctx* diag) {
    static const char spaces[] = "                                                                ";

    while (count > 0u) {
        size_t chunk = count < sizeof(spaces) - 1u ? count : sizeof(spaces) - 1u;
        if (!bx_line_writer_write(writer, spaces, chunk)) {
            return bx_nl_write_error(diag);
        }
        count -= chunk;
    }

    return true;
}

static bool bx_nl_write_number_field(struct bx_line_writer* writer, long long num, struct bx_nl_options* options, struct bx_diag_ctx* diag) {
    char number_buffer[64];
    int len = snprintf(number_buffer, sizeof(number_buffer), "%lld", num);
    if (len < 0 || (size_t)len >= sizeof(number_buffer)) {
        errno = EIO;
        return bx_nl_write_error(diag);
    }

    size_t number_len = (size_t)len;
    long long width_value = options->number_width;
    bool left_adjust = width_value < 0;
    bool zero_pad = false;

    if (strcmp(options->number_format, "ln") == 0) {
        left_adjust = true;
    }
    else if (strcmp(options->number_format, "rz") == 0 && width_value >= 0) {
        zero_pad = true;
    }

    size_t width = (size_t)(width_value < 0 ? -width_value : width_value);
    size_t pad = width > number_len ? width - number_len : 0u;

    if (!left_adjust && !zero_pad && !bx_nl_write_padding(writer, pad, diag)) {
        return false;
    }

    if (zero_pad && number_buffer[0] == '-') {
        if (!bx_line_writer_putc(writer, '-')) {
            return bx_nl_write_error(diag);
        }
        if (!bx_nl_write_padding(writer, pad, diag)) {
            return false;
        }
        if (!bx_line_writer_write(writer, number_buffer + 1, number_len - 1u)) {
            return bx_nl_write_error(diag);
        }
    }
    else {
        if (zero_pad) {
            static const char zeros[] = "0000000000000000000000000000000000000000000000000000000000000000";
            size_t remaining = pad;
            while (remaining > 0u) {
                size_t chunk = remaining < sizeof(zeros) - 1u ? remaining : sizeof(zeros) - 1u;
                if (!bx_line_writer_write(writer, zeros, chunk)) {
                    return bx_nl_write_error(diag);
                }
                remaining -= chunk;
            }
        }
        if (!bx_line_writer_write(writer, number_buffer, number_len)) {
            return bx_nl_write_error(diag);
        }
    }

    if (left_adjust && !bx_nl_write_padding(writer, pad, diag)) {
        return false;
    }

    if (!bx_line_writer_puts(writer, options->number_separator)) {
        return bx_nl_write_error(diag);
    }

    return true;
}

static bool bx_nl_write_unnumbered_prefix(struct bx_line_writer* writer, struct bx_nl_options* options, struct bx_diag_ctx* diag) {
    long long width = (long long)options->number_width + (long long)strlen(options->number_separator);
    if (width < 0) {
        width = -width;
    }
    return bx_nl_write_padding(writer, (size_t)width, diag);
}

static bool nl_file(FILE* f, struct bx_nl_options* options, struct bx_line_writer* writer, struct bx_diag_ctx* diag) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    long long current_line = options->starting_line_number;
    bool advance_number = false;
    struct section_style* current_style = &options->body;
    int blank_count = 0;

    char delim1 = options->section_delim[0];
    char delim2 = options->section_delim[1];

    while ((len = getline(&line, &line_cap, f)) != -1) {
        // Check for section changes
        if (line[0] == delim1 && line[1] == delim2) {
            if (line[2] == delim1 && line[3] == delim2 && line[4] == delim1 && line[5] == delim2 && (line[6] == '\n' || line[6] == '\0')) {
                // Header \:\:\:
                current_style = &options->header;
                if (!options->no_renumber) {
                    current_line = options->starting_line_number;
                    advance_number = false;
                }
                if (!bx_line_writer_putc(writer, '\n')) {
                    free(line);
                    return bx_nl_write_error(diag);
                }
                continue;
            }
            else if (line[2] == delim1 && line[3] == delim2 && (line[4] == '\n' || line[4] == '\0')) {
                // Body \:\:
                current_style = &options->body;
                if (!options->no_renumber) {
                    current_line = options->starting_line_number;
                    advance_number = false;
                }
                if (!bx_line_writer_putc(writer, '\n')) {
                    free(line);
                    return bx_nl_write_error(diag);
                }
                continue;
            }
            else if (line[2] == '\n' || line[2] == '\0') {
                // Footer \:
                current_style = &options->footer;
                if (!options->no_renumber) {
                    current_line = options->starting_line_number;
                    advance_number = false;
                }
                if (!bx_line_writer_putc(writer, '\n')) {
                    free(line);
                    return bx_nl_write_error(diag);
                }
                continue;
            }
        }

        bool should_number = false;
        bool is_blank = (line[0] == '\n' || line[0] == '\0');

        switch (current_style->style) {
            case STYLE_ALL:
                should_number = true;
                break;
            case STYLE_NONEMPTY:
                should_number = !is_blank;
                break;
            case STYLE_NONE:
                should_number = false;
                break;
            case STYLE_REGEX:
                if (!is_blank && regexec(&current_style->regex, line, 0, NULL, 0) == 0)
                    should_number = true;
                break;
        }

        if (is_blank && should_number) {
            blank_count++;
            if (blank_count < options->join_blank_lines) {
                should_number = false;
            }
            else {
                blank_count = 0;
            }
        }
        else if (!is_blank) {
            blank_count = 0;
        }

        if (should_number) {
            if (advance_number && __builtin_add_overflow(current_line, options->line_increment, &current_line)) {
                bx_diag(diag, "line number overflow");
                free(line);
                return false;
            }
            if (!bx_nl_write_number_field(writer, current_line, options, diag)) {
                free(line);
                return false;
            }
            advance_number = true;
        }
        else {
            if (!bx_nl_write_unnumbered_prefix(writer, options, diag)) {
                free(line);
                return false;
            }
        }
        if (!bx_line_writer_puts(writer, line)) {
            free(line);
            return bx_nl_write_error(diag);
        }
    }

    free(line);
    return true;
}

int bx_nl_main(int argc, char** argv) {
    struct bx_nl_options options;
    struct bx_diag_ctx diag = {.progname = "nl", .exit_status = 0};

    if (!bx_nl_parse_options(argc, argv, &options, &diag)) {
        if (diag.exit_status == 0)
            diag.exit_status = 1;
        goto done;
    }
    if (options.show_help) {
        bx_nl_print_help(stdout, options.progname);
        goto done;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        goto done;
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    size_t num_files = options.file_count;
    for (size_t i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : options.files[i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }
        bool ok = nl_file(f, &options, &writer, &diag);
        bx_fclose_nonstdio(f, is_stdio);
        if (!ok) {
            break;
        }
    }

    if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_nl_write_error(&diag);
    }

done:
    clear_style(&options.body);
    clear_style(&options.header);
    clear_style(&options.footer);
    free(options.files);
    return diag.exit_status;
}
