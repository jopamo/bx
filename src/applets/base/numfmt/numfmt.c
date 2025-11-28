#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <locale.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum unit_type { UNIT_NONE, UNIT_SI, UNIT_IEC, UNIT_IEC_I, UNIT_AUTO };

struct bx_numfmt_options {
    const char* progname;
    enum unit_type from_unit;
    enum unit_type to_unit;
    int padding;
    int header;
    int field;
    char delimiter;
    const char* suffix;
    const char* format;
    bool grouping;
    bool show_help;
    bool show_version;
};

static void bx_numfmt_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [NUMBER]...\n", progname);
    fprintf(stream, "Reformat NUMBER(s), or numbers from standard input if none are specified.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  --from=UNIT        auto-scale input numbers to UNITs; default is 'none';\n");
    fprintf(stream, "                       see UNIT below\n");
    fprintf(stream, "  --to=UNIT          auto-scale output numbers to UNITs; default is 'none';\n");
    fprintf(stream, "                       see UNIT below\n");
    fprintf(stream, "  --padding=N        pad the output to N characters; positive N will\n");
    fprintf(stream, "                       right-align; negative N will left-align\n");
    fprintf(stream, "  --header[=N]       print (without converting) the first N header lines;\n");
    fprintf(stream, "                       default is 1 if N is omitted\n");
    fprintf(stream, "  --field=N          replace the number in input field N (default is 1)\n");
    fprintf(stream, "  --delimiter=C      use C as the field separator instead of whitespace\n");
    fprintf(stream, "  --suffix=S         add S to output numbers and accept S in input numbers\n");
    fprintf(stream, "  --format=F         use printf-style floating-point format F\n");
    fprintf(stream, "  --grouping         use locale-defined grouping of digits, e.g. 1,000,000\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "UNIT options:\n");
    fprintf(stream, "  none       no auto-scaling is done; suffixes will trigger an error\n");
    fprintf(stream, "  auto       accept optional single-letter or two-letter suffix:\n");
    fprintf(stream, "               1K = 1000, 1Ki = 1024, 1M = 1000000, 1Mi = 1048576, ...\n");
    fprintf(stream, "  si         accept optional single-letter suffix:\n");
    fprintf(stream, "               1K = 1000, 1M = 1000000, ...\n");
    fprintf(stream, "  iec        accept optional single-letter suffix:\n");
    fprintf(stream, "               1K = 1024, 1M = 1048576, ...\n");
    fprintf(stream, "  iec-i      accept optional two-letter suffix:\n");
    fprintf(stream, "               1Ki = 1024, 1Mi = 1048576, ...\n");
}

static enum unit_type parse_unit(const char* str, struct bx_diag_ctx* diag) {
    if (strcmp(str, "none") == 0)
        return UNIT_NONE;
    if (strcmp(str, "si") == 0)
        return UNIT_SI;
    if (strcmp(str, "iec") == 0)
        return UNIT_IEC;
    if (strcmp(str, "iec-i") == 0)
        return UNIT_IEC_I;
    if (strcmp(str, "auto") == 0)
        return UNIT_AUTO;
    bx_diag(diag, "invalid unit: '%s'", str);
    return UNIT_NONE;
}

static bool bx_numfmt_parse_options(int argc, char** argv, struct bx_numfmt_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"from", required_argument, NULL, 1},  {"to", required_argument, NULL, 2},        {"padding", required_argument, NULL, 3}, {"header", optional_argument, NULL, 4},
        {"field", required_argument, NULL, 5}, {"delimiter", required_argument, NULL, 6}, {"suffix", required_argument, NULL, 7},  {"format", required_argument, NULL, 8},
        {"grouping", no_argument, NULL, 9},    {"help", no_argument, NULL, 10},           {"version", no_argument, NULL, 11},      {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "numfmt";
    options->field = 1;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
            case 1:
                options->from_unit = parse_unit(optarg, diag);
                break;
            case 2:
                options->to_unit = parse_unit(optarg, diag);
                break;
            case 3:
                options->padding = atoi(optarg);
                break;
            case 4:
                options->header = optarg ? atoi(optarg) : 1;
                break;
            case 5:
                options->field = atoi(optarg);
                break;
            case 6:
                options->delimiter = optarg[0];
                break;
            case 7:
                options->suffix = optarg;
                break;
            case 8:
                options->format = optarg;
                break;
            case 9:
                options->grouping = true;
                break;
            case 10:
                options->show_help = true;
                return true;
            case 11:
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

static double parse_number(const char* str, enum unit_type from, struct bx_diag_ctx* diag) {
    char* endptr;
    double val = strtod(str, &endptr);
    if (endptr == str) {
        bx_diag(diag, "invalid number: '%s'", str);
        return NAN;
    }

    if (*endptr != '\0') {
        const char* s = endptr;
        double factor = 1.0;
        double base = (from == UNIT_SI) ? 1000.0 : 1024.0;

        const char* units = "KMGTP EZY";
        const char* p = strchr(units, toupper((unsigned char)*s));
        if (p) {
            int exp = (int)(p - units) + 1;
            factor = pow(base, exp);
            s++;
            if (from == UNIT_IEC_I || from == UNIT_AUTO) {
                if (*s == 'i')
                    s++;
            }
        }
        val *= factor;
    }
    return val;
}

static void print_with_padding(const char* str, int padding) {
    if (padding > 0)
        printf("%*s", padding, str);
    else if (padding < 0)
        printf("%-*s", -padding, str);
    else
        printf("%s", str);
}

static void format_number(double val, struct bx_numfmt_options* options) {
    char buf[128];
    if (options->to_unit == UNIT_NONE) {
        if (options->format)
            sprintf(buf, options->format, val);
        else
            sprintf(buf, "%.0f", val);
        print_with_padding(buf, options->padding);
    }
    else {
        double base = (options->to_unit == UNIT_SI) ? 1000.0 : 1024.0;
        const char* units = " KMGTP EZY";
        int exp = 0;
        if (val >= base || val <= -base) {
            exp = (int)(log(fabs(val)) / log(base));
            if (exp > 8)
                exp = 8;
            val /= pow(base, exp);
        }

        if (options->format)
            sprintf(buf, options->format, val);
        else
            sprintf(buf, "%.1f", val);

        if (units[exp] != ' ') {
            size_t len = strlen(buf);
            buf[len] = units[exp];
            if (options->to_unit == UNIT_IEC_I) {
                buf[len + 1] = 'i';
                buf[len + 2] = '\0';
            }
            else {
                buf[len + 1] = '\0';
            }
        }
        print_with_padding(buf, options->padding);
    }
}

static void process_line(char* line, struct bx_numfmt_options* options, struct bx_diag_ctx* diag) {
    if (options->field == 1 && options->delimiter == 0) {
        double val = parse_number(line, options->from_unit, diag);
        if (!isnan(val)) {
            format_number(val, options);
            printf("\n");
        }
        else {
            printf("%s", line);
        }
    }
    else {
        printf("%s", line);
    }
}

int bx_numfmt_main(int argc, char** argv) {
    struct bx_numfmt_options options;
    struct bx_diag_ctx diag = {.progname = "numfmt", .exit_status = 0};
    int first_operand = 0;

    setlocale(LC_ALL, "");

    if (!bx_numfmt_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_numfmt_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int num_args = argc - first_operand;
    if (num_args > 0) {
        for (int i = 0; i < num_args; i++) {
            double val = parse_number(argv[first_operand + i], options.from_unit, &diag);
            if (!isnan(val))
                format_number(val, &options);
            printf("\n");
        }
    }
    else {
        char* line = NULL;
        size_t len = 0;
        int header_count = 0;
        while (getline(&line, &len, stdin) != -1) {
            if (header_count < options.header) {
                printf("%s", line);
                header_count++;
                continue;
            }
            process_line(line, &options, &diag);
        }
        free(line);
    }

    return diag.exit_status;
}
