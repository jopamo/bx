#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
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

static bool parse_style(const char* str, struct section_style* style, struct bx_diag_ctx* diag) {
    if (str[0] == 'a')
        style->style = STYLE_ALL;
    else if (str[0] == 't')
        style->style = STYLE_NONEMPTY;
    else if (str[0] == 'n')
        style->style = STYLE_NONE;
    else if (str[0] == 'p') {
        style->style = STYLE_REGEX;
        if (regcomp(&style->regex, str + 1, REG_NOSUB) != 0) {
            bx_diag(diag, "invalid regular expression: %s", str + 1);
            return false;
        }
    }
    else {
        bx_diag(diag, "invalid body numbering style: '%s'", str);
        return false;
    }
    return true;
}

static bool bx_nl_parse_options(int argc, char** argv, struct bx_nl_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"body-numbering", required_argument, NULL, 'b'},
        {"section-delimiter", required_argument, NULL, 'd'},
        {"footer-numbering", required_argument, NULL, 'f'},
        {"header-numbering", required_argument, NULL, 'h'},
        {"line-increment", required_argument, NULL, 'i'},
        {"join-blank-lines", required_argument, NULL, 'l'},
        {"number-format", required_argument, NULL, 'n'},
        {"no-renumber", no_argument, NULL, 'p'},
        {"number-separator", required_argument, NULL, 's'},
        {"starting-line-number", required_argument, NULL, 'v'},
        {"number-width", required_argument, NULL, 'w'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
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

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "b:d:f:h:i:l:n:ps:v:w:", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 'b':
                if (!parse_style(optarg, &options->body, diag))
                    return false;
                break;
            case 'd':
                if (strlen(optarg) == 1) {
                    options->section_delim[0] = optarg[0];
                    options->section_delim[1] = ':';
                }
                else if (strlen(optarg) == 2) {
                    options->section_delim[0] = optarg[0];
                    options->section_delim[1] = optarg[1];
                }
                else {
                    bx_diag(diag, "invalid section delimiter: '%s'", optarg);
                    return false;
                }
                break;
            case 'f':
                if (!parse_style(optarg, &options->footer, diag))
                    return false;
                break;
            case 'h':
                if (!parse_style(optarg, &options->header, diag))
                    return false;
                break;
            case 'i':
                if (!bx_args_parse_int_range(optarg, INT_MIN, INT_MAX, &options->line_increment)) {
                    bx_diag(diag, "invalid line increment: '%s'", optarg);
                    return false;
                }
                break;
            case 'l':
                if (!bx_args_parse_int_range(optarg, INT_MIN, INT_MAX, &options->join_blank_lines)) {
                    bx_diag(diag, "invalid blank line count: '%s'", optarg);
                    return false;
                }
                break;
            case 'n':
                options->number_format = optarg;
                break;
            case 'p':
                options->no_renumber = true;
                break;
            case 's':
                options->number_separator = optarg;
                break;
            case 'v':
                if (!bx_args_parse_llong_range(optarg, LLONG_MIN, LLONG_MAX, &options->starting_line_number)) {
                    bx_diag(diag, "invalid starting line number: '%s'", optarg);
                    return false;
                }
                break;
            case 'w':
                if (!bx_args_parse_int_range(optarg, INT_MIN, INT_MAX, &options->number_width)) {
                    bx_diag(diag, "invalid number width: '%s'", optarg);
                    return false;
                }
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

    *first_operand = optind;
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
                if (!options->no_renumber)
                    current_line = options->starting_line_number;
                if (!bx_line_writer_putc(writer, '\n')) {
                    free(line);
                    return bx_nl_write_error(diag);
                }
                continue;
            }
            else if (line[2] == delim1 && line[3] == delim2 && (line[4] == '\n' || line[4] == '\0')) {
                // Body \:\:
                current_style = &options->body;
                if (!options->no_renumber)
                    current_line = options->starting_line_number;
                if (!bx_line_writer_putc(writer, '\n')) {
                    free(line);
                    return bx_nl_write_error(diag);
                }
                continue;
            }
            else if (line[2] == '\n' || line[2] == '\0') {
                // Footer \:
                current_style = &options->footer;
                if (!options->no_renumber)
                    current_line = options->starting_line_number;
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
            if (!bx_nl_write_number_field(writer, current_line, options, diag)) {
                free(line);
                return false;
            }
            current_line += options->line_increment;
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
    int first_operand = 0;

    if (!bx_nl_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_nl_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    int num_files = argc - first_operand;
    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
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

    if (options.body.style == STYLE_REGEX)
        regfree(&options.body.regex);
    if (options.header.style == STYLE_REGEX)
        regfree(&options.header.regex);
    if (options.footer.style == STYLE_REGEX)
        regfree(&options.footer.regex);

    return diag.exit_status;
}
