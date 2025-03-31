#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

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

static void bx_nl_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "b:d:f:h:i:l:n:ps:v:w:", long_options, &option_index);
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
                options->line_increment = atoi(optarg);
                break;
            case 'l':
                options->join_blank_lines = atoi(optarg);
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
                options->starting_line_number = atoll(optarg);
                break;
            case 'w':
                options->number_width = atoi(optarg);
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

static void print_number(long long num, struct bx_nl_options* options) {
    char fmt[32];
    if (strcmp(options->number_format, "ln") == 0) {
        sprintf(fmt, "%%-%lldlld%%s", (long long)options->number_width);
        printf(fmt, num, options->number_separator);
    }
    else if (strcmp(options->number_format, "rn") == 0) {
        sprintf(fmt, "%%%lldlld%%s", (long long)options->number_width);
        printf(fmt, num, options->number_separator);
    }
    else if (strcmp(options->number_format, "rz") == 0) {
        sprintf(fmt, "%%0%lldlld%%s", (long long)options->number_width);
        printf(fmt, num, options->number_separator);
    }
    else {
        // Fallback
        printf("%*lld%s", options->number_width, num, options->number_separator);
    }
}

static void nl_file(FILE* f, struct bx_nl_options* options) {
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
                continue;
            }
            else if (line[2] == delim1 && line[3] == delim2 && (line[4] == '\n' || line[4] == '\0')) {
                // Body \:\:
                current_style = &options->body;
                if (!options->no_renumber)
                    current_line = options->starting_line_number;
                continue;
            }
            else if (line[2] == '\n' || line[2] == '\0') {
                // Footer \:
                current_style = &options->footer;
                if (!options->no_renumber)
                    current_line = options->starting_line_number;
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
            print_number(current_line, options);
            current_line += options->line_increment;
        }
        else {
            printf("%*s", options->number_width + (int)strlen(options->number_separator), "");
        }
        printf("%s", line);
    }
    free(line);
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
        bx_nl_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        FILE* f = strcmp(filename, "-") == 0 ? stdin : fopen(filename, "r");
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }
        nl_file(f, &options);
        if (f != stdin)
            fclose(f);
    }

    if (options.body.style == STYLE_REGEX)
        regfree(&options.body.regex);
    if (options.header.style == STYLE_REGEX)
        regfree(&options.header.regex);
    if (options.footer.style == STYLE_REGEX)
        regfree(&options.footer.regex);

    return diag.exit_status;
}
