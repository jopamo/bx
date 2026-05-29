#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/args_common.h"

enum cut_mode { CUT_MODE_BYTES, CUT_MODE_CHARS, CUT_MODE_FIELDS, CUT_MODE_NONE };

struct range {
    size_t start;
    size_t end;
};

struct bx_cut_options {
    const char* progname;
    enum cut_mode mode;
    struct range* ranges;
    size_t num_ranges;
    char delimiter;
    bool only_delimited;
    bool complement;
    const char* output_delimiter;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_cut_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s OPTION... [FILE]...\n", progname);
    fprintf(stream, "Print selected parts of lines from each FILE to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -b, --bytes=LIST         select only these bytes\n");
    fprintf(stream, "  -c, --characters=LIST    select only these characters\n");
    fprintf(stream, "  -d, --delimiter=DELIM    use DELIM instead of TAB for field delimiter\n");
    fprintf(stream, "  -f, --fields=LIST        select only these fields; also print any line\n");
    fprintf(stream, "                             that contains no delimiter character, unless\n");
    fprintf(stream, "                             the -s option is specified\n");
    fprintf(stream, "  -n                       (ignored)\n");
    fprintf(stream, "      --complement         complement the set of selected bytes, characters\n");
    fprintf(stream, "                             or fields\n");
    fprintf(stream, "  -s, --only-delimited     do not print lines not containing delimiters\n");
    fprintf(stream, "      --output-delimiter=STRING  use STRING as the output delimiter\n");
    fprintf(stream, "                            the default is to use the input delimiter\n");
    fprintf(stream, "  -z, --zero-terminated    line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Use one, and only one of -b, -c or -f.  Each LIST is made up of one\n");
    fprintf(stream, "range, or many ranges separated by commas.  Selected input is written\n");
    fprintf(stream, "in the same order that it is read, and is written exactly once.\n");
    fprintf(stream, "Each range is one of:\n");
    fprintf(stream, "\n");
    fprintf(stream, "  N     N'th byte, character or field, counted from 1\n");
    fprintf(stream, "  N-    from N'th byte, character or field, to end of line\n");
    fprintf(stream, "  N-M   from N'th to M'th (inclusive) byte, character or field\n");
    fprintf(stream, "  -M    from first to M'th (inclusive) byte, character or field\n");
}

static int compare_ranges(const void* a, const void* b) {
    const struct range* ra = a;
    const struct range* rb = b;
    if (ra->start < rb->start)
        return -1;
    if (ra->start > rb->start)
        return 1;
    return 0;
}

static void bx_cut_free_ranges(struct bx_cut_options* options) {
    free(options->ranges);
    options->ranges = NULL;
    options->num_ranges = 0;
}

static bool cut_parse_list_number(const char* text, size_t* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-' || value_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || end == NULL || *end != '\0' ||
        value == 0 || value > (uintmax_t)SIZE_MAX) {
        return false;
    }

    *value_out = (size_t)value;
    return true;
}

static bool parse_list(const char* list, struct bx_cut_options* options, struct bx_diag_ctx* diag) {
    char* copy = xstrdup(list);
    size_t count = 1;
    for (const char* c = list; *c; c++)
        if (*c == ',')
            count++;

    options->ranges = xmalloc(count * sizeof(struct range));
    options->num_ranges = 0;

    char* tok_start = copy;
    while (tok_start) {
        char* tok_end = strchr(tok_start, ',');
        if (tok_end)
            *tok_end = '\0';

        if (*tok_start != '\0') {
            struct range r = {0, SIZE_MAX};
            char* dash = strchr(tok_start, '-');
            if (dash) {
                *dash = '\0';
                char* start_s = tok_start;
                char* end_s = dash + 1;

                if (*start_s != '\0') {
                    size_t val = 0;
                    if (!cut_parse_list_number(start_s, &val)) {
                        bx_diag(diag, "invalid byte, character or field list");
                        bx_cut_free_ranges(options);
                        free(copy);
                        return false;
                    }
                    r.start = val;
                }
                else {
                    r.start = 1;
                }

                if (*end_s != '\0') {
                    size_t val = 0;
                    if (!cut_parse_list_number(end_s, &val)) {
                        bx_diag(diag, "invalid byte, character or field list");
                        bx_cut_free_ranges(options);
                        free(copy);
                        return false;
                    }
                    r.end = val;
                }
            }
            else {
                size_t val = 0;
                if (!cut_parse_list_number(tok_start, &val)) {
                    bx_diag(diag, "invalid byte, character or field list");
                    bx_cut_free_ranges(options);
                    free(copy);
                    return false;
                }
                r.start = r.end = val;
            }

            if (r.start > r.end && r.end != SIZE_MAX) {
                bx_diag(diag, "invalid decreasing range");
                bx_cut_free_ranges(options);
                free(copy);
                return false;
            }

            options->ranges[options->num_ranges++] = r;
        }

        tok_start = tok_end ? tok_end + 1 : NULL;
    }

    free(copy);

    if (options->num_ranges == 0) {
        bx_diag(diag, "missing list of fields");
        bx_cut_free_ranges(options);
        return false;
    }

    // Sort and merge ranges
    qsort(options->ranges, options->num_ranges, sizeof(struct range), compare_ranges);

    size_t j = 0;
    for (size_t i = 1; i < options->num_ranges; i++) {
        if (options->ranges[i].start <= options->ranges[j].end || (options->ranges[j].end != SIZE_MAX && options->ranges[i].start == options->ranges[j].end + 1)) {
            if (options->ranges[i].end > options->ranges[j].end || options->ranges[i].end == SIZE_MAX) {
                options->ranges[j].end = options->ranges[i].end;
            }
        }
        else {
            j++;
            options->ranges[j] = options->ranges[i];
        }
    }
    options->num_ranges = j + 1;

    return true;
}

static bool bx_cut_parse_options(int argc, char** argv, struct bx_cut_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"bytes", required_argument, NULL, 'b'},
        {"characters", required_argument, NULL, 'c'},
        {"delimiter", required_argument, NULL, 'd'},
        {"fields", required_argument, NULL, 'f'},
        {"complement", no_argument, NULL, 1},
        {"only-delimited", no_argument, NULL, 's'},
        {"output-delimiter", required_argument, NULL, 2},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 3},
        {"version", no_argument, NULL, 4},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "cut";
    options->mode = CUT_MODE_NONE;
    options->delimiter = '\t';
    diag->progname = options->progname;

    const char* list_str = NULL;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+b:c:d:f:nsz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'b':
                if (options->mode != CUT_MODE_NONE) {
                    bx_diag(diag, "only one type of list may be specified");
                    return false;
                }
                options->mode = CUT_MODE_BYTES;
                list_str = optarg;
                break;
            case 'c':
                if (options->mode != CUT_MODE_NONE) {
                    bx_diag(diag, "only one type of list may be specified");
                    return false;
                }
                options->mode = CUT_MODE_CHARS;
                list_str = optarg;
                break;
            case 'f':
                if (options->mode != CUT_MODE_NONE) {
                    bx_diag(diag, "only one type of list may be specified");
                    return false;
                }
                options->mode = CUT_MODE_FIELDS;
                list_str = optarg;
                break;
            case 'd':
                if (strlen(optarg) != 1) {
                    bx_diag(diag, "the delimiter must be a single character");
                    return false;
                }
                options->delimiter = optarg[0];
                break;
            case 'n':
                break;  // ignored
            case 's':
                options->only_delimited = true;
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case 1:
                options->complement = true;
                break;
            case 2:
                options->output_delimiter = optarg;
                break;
            case 3:
                options->show_help = true;
                return true;
            case 4:
                options->show_version = true;
                return true;
            case '?':
                bx_diag(diag, "invalid option -- '%c'", optopt);
                return false;
            default:
                return false;
        }
    }

    if (options->mode == CUT_MODE_NONE) {
        bx_diag(diag, "you must specify a list of bytes, characters, or fields");
        return false;
    }

    if (!parse_list(list_str, options, diag))
        return false;

    if (!options->output_delimiter) {
        static char delim_str[2] = {0, 0};
        delim_str[0] = options->delimiter;
        options->output_delimiter = (options->mode == CUT_MODE_FIELDS) ? delim_str : "";
    }

    *first_operand = optind;
    return true;
}

static bool is_selected(size_t pos, struct bx_cut_options* options) {
    for (size_t i = 0; i < options->num_ranges; i++) {
        if (pos >= options->ranges[i].start && pos <= options->ranges[i].end) {
            return !options->complement;
        }
    }
    return options->complement;
}

static void cut_line(char* line, ssize_t len, struct bx_cut_options* options) {
    if (len > 0 && line[len - 1] == (options->zero_terminated ? '\0' : '\n')) {
        len--;
    }

    if (options->mode == CUT_MODE_BYTES || options->mode == CUT_MODE_CHARS) {
        // Character mode is same as byte mode for now (UTF-8 support not implemented)
        for (ssize_t i = 0; i < len; i++) {
            if (is_selected((size_t)i + 1, options)) {
                putchar(line[i]);
            }
        }
        putchar(options->zero_terminated ? '\0' : '\n');
    }
    else {
        // Fields mode
        size_t field_idx = 1;
        bool first = true;
        bool found_delimiter = false;

        // First pass: check if delimiter exists
        for (ssize_t i = 0; i < len; i++) {
            if (line[i] == options->delimiter) {
                found_delimiter = true;
                break;
            }
        }

        if (!found_delimiter) {
            if (!options->only_delimited) {
                fwrite(line, 1, (size_t)len, stdout);
                putchar(options->zero_terminated ? '\0' : '\n');
            }
            return;
        }

        char* start = line;
        for (ssize_t i = 0; i <= len; i++) {
            if (i == len || line[i] == options->delimiter) {
                if (is_selected(field_idx, options)) {
                    if (!first)
                        fputs(options->output_delimiter, stdout);
                    fwrite(start, 1, (size_t)(&line[i] - start), stdout);
                    first = false;
                }
                start = &line[i + 1];
                field_idx++;
            }
        }
        putchar(options->zero_terminated ? '\0' : '\n');
    }
}

int bx_cut_main(int argc, char** argv) {
    struct bx_cut_options options;
    struct bx_diag_ctx diag = {.progname = "cut", .exit_status = 0};
    int first_operand = 0;

    if (!bx_cut_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_cut_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int num_files = argc - first_operand;
    int delimiter = options.zero_terminated ? '\0' : '\n';

    char* line = NULL;
    size_t line_cap = 0;

    for (int i = 0; i < num_files || (i == 0 && num_files == 0); i++) {
        const char* filename = (num_files == 0) ? "-" : argv[first_operand + i];
        bool is_stdio = false;
        FILE* f = bx_fopen_dash(filename, "r", &is_stdio);
        if (!f) {
            bx_diag(&diag, "%s: %s", filename, strerror(errno));
            continue;
        }

        ssize_t len;
        while ((len = getdelim(&line, &line_cap, delimiter, f)) != -1) {
            cut_line(line, len, &options);
        }

        bx_fclose_nonstdio(f, is_stdio);
    }

    free(line);
    bx_cut_free_ranges(&options);
    return diag.exit_status;
}
