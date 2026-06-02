#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "lib/args_common.h"

enum unit_type { UNIT_NONE, UNIT_SI, UNIT_IEC, UNIT_IEC_I, UNIT_AUTO };

enum bx_numfmt_parse_status {
    BX_NUMFMT_PARSE_OK = 0,
    BX_NUMFMT_PARSE_ERROR = 1,
    BX_NUMFMT_PARSE_ERROR_TRY_HELP = 2,
};

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

static bool bx_numfmt_parse_unit(const char* str, enum unit_type* unit_out, struct bx_diag_ctx* diag) {
    if (strcmp(str, "none") == 0) {
        *unit_out = UNIT_NONE;
        return true;
    }
    if (strcmp(str, "si") == 0) {
        *unit_out = UNIT_SI;
        return true;
    }
    if (strcmp(str, "iec") == 0) {
        *unit_out = UNIT_IEC;
        return true;
    }
    if (strcmp(str, "iec-i") == 0) {
        *unit_out = UNIT_IEC_I;
        return true;
    }
    if (strcmp(str, "auto") == 0) {
        *unit_out = UNIT_AUTO;
        return true;
    }

    bx_diag(diag, "invalid unit: '%s'", str);
    return false;
}

static bool bx_numfmt_parse_padding(const char* text, int* value_out, struct bx_diag_ctx* diag) {
    intmax_t parsed = 0;
    if (!bx_size_parse_signed_count(text, &parsed) || parsed < INT_MIN || parsed > INT_MAX) {
        bx_diag(diag, "invalid padding value '%s'", text);
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool bx_numfmt_parse_header_count(const char* text, int* value_out, struct bx_diag_ctx* diag) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_uint(text, &parsed) || parsed > INT_MAX) {
        bx_diag(diag, "invalid header value '%s'", text);
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool bx_numfmt_parse_field_index(const char* text, int* value_out, struct bx_diag_ctx* diag) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_uint(text, &parsed) || parsed == 0 || parsed > INT_MAX) {
        bx_diag(diag, "field value must be a positive integer: '%s'", text);
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static bool bx_numfmt_long_option_requires_arg(const char* arg) {
    static const char* required[] = {
        "--from",
        "--to",
        "--padding",
        "--field",
        "--delimiter",
        "--suffix",
        "--format",
    };

    for (size_t i = 0; i < (sizeof(required) / sizeof(required[0])); i++) {
        if (strcmp(arg, required[i]) == 0) {
            return true;
        }
    }

    return false;
}

static const char* bx_numfmt_missing_arg_name(int opt_value) {
    switch (opt_value) {
        case 1:
            return "--from";
        case 2:
            return "--to";
        case 3:
            return "--padding";
        case 5:
            return "--field";
        case 6:
            return "--delimiter";
        case 7:
            return "--suffix";
        case 8:
            return "--format";
        default:
            return NULL;
    }
}

static enum bx_numfmt_parse_status bx_numfmt_parse_options(
    int argc,
    char** argv,
    struct bx_numfmt_options* options,
    int* first_operand,
    struct bx_diag_ctx* diag
) {
    static const struct option long_options[] = {
        {"from", required_argument, NULL, 1},
        {"to", required_argument, NULL, 2},
        {"padding", required_argument, NULL, 3},
        {"header", optional_argument, NULL, 4},
        {"field", required_argument, NULL, 5},
        {"delimiter", required_argument, NULL, 6},
        {"suffix", required_argument, NULL, 7},
        {"format", required_argument, NULL, 8},
        {"grouping", no_argument, NULL, 9},
        {"help", no_argument, NULL, 10},
        {"version", no_argument, NULL, 11},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "numfmt");
    options->field = 1;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                if (!bx_numfmt_parse_unit(optarg, &options->from_unit, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 2:
                if (!bx_numfmt_parse_unit(optarg, &options->to_unit, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 3:
                if (!bx_numfmt_parse_padding(optarg, &options->padding, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 4:
                if (optarg == NULL) {
                    options->header = 1;
                    break;
                }
                if (!bx_numfmt_parse_header_count(optarg, &options->header, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 5:
                if (!bx_numfmt_parse_field_index(optarg, &options->field, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 6:
                if (optarg[0] == '\0' || optarg[1] != '\0') {
                    bx_diag(diag, "delimiter must be a single character: '%s'", optarg);
                    return BX_NUMFMT_PARSE_ERROR;
                }
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
                return BX_NUMFMT_PARSE_OK;
            case 11:
                options->show_version = true;
                return BX_NUMFMT_PARSE_OK;
            case '?': {
                const char* missing_name = bx_numfmt_missing_arg_name(optopt);
                if (missing_name != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", missing_name);
                    return BX_NUMFMT_PARSE_ERROR_TRY_HELP;
                }
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    const char* current = argv[optind - 1];
                    if (strncmp(current, "--", 2) == 0 && bx_numfmt_long_option_requires_arg(current)) {
                        bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                    }
                    else {
                        bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                    }
                }
                else {
                    bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                }
                return BX_NUMFMT_PARSE_ERROR_TRY_HELP;
            }
            default:
                return BX_NUMFMT_PARSE_ERROR;
        }
    }

    *first_operand = optind;
    return BX_NUMFMT_PARSE_OK;
}

static bool bx_numfmt_is_field_space(unsigned char ch) {
    return ch != '\n' && isspace(ch);
}

static bool bx_numfmt_parse_suffix(const char* suffix, enum unit_type from, double* value_out) {
    if (suffix == NULL || suffix[0] == '\0') {
        return true;
    }

    if (from == UNIT_NONE) {
        return false;
    }

    unsigned int power = 0u;
    char unit_char = (char)toupper((unsigned char)suffix[0]);
    if (!bx_size_suffix_prefix_power(unit_char, &power) || power > 8u) {
        return false;
    }

    bool has_i = false;
    const char* rest = suffix + 1;
    if (*rest == 'i' || *rest == 'I') {
        has_i = true;
        rest++;
    }

    if (*rest != '\0') {
        return false;
    }

    if (from == UNIT_SI && has_i) {
        return false;
    }
    if (from == UNIT_IEC_I && !has_i) {
        return false;
    }

    enum bx_size_unit_label_style style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
    if (from == UNIT_AUTO) {
        style = has_i ? BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX : BX_SIZE_UNIT_LABEL_SI_LOWER_K;
    }
    else if (from == UNIT_SI) {
        style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
    }
    else {
        style = has_i ? BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX : BX_SIZE_UNIT_LABEL_IEC_PREFIX;
    }

    double base = 0.0;
    if (!bx_size_unit_label_base_double(style, &base)) {
        return false;
    }

    double multiplier = 1.0;
    if (!bx_size_power_double(base, power, &multiplier)) {
        return false;
    }
    *value_out *= multiplier;
    return true;
}

static bool bx_numfmt_parse_float_value(const char* text, double* value_out, char** end_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL || end_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (end == text) {
        return false;
    }
    if (errno != 0 && errno != ERANGE) {
        return false;
    }

    *value_out = value;
    *end_out = end;
    return true;
}

static double bx_numfmt_parse_number_text(
    const char* text,
    size_t len,
    const struct bx_numfmt_options* options,
    struct bx_diag_ctx* diag,
    bool* ok_out,
    bool* fatal_out
) {
    *ok_out = false;
    if (fatal_out != NULL) {
        *fatal_out = false;
    }

    if (len > SIZE_MAX - 1u) {
        bx_diag(diag, "out of memory");
        if (fatal_out != NULL) {
            *fatal_out = true;
        }
        return NAN;
    }

    char* scratch = malloc(len + 1);
    if (scratch == NULL) {
        bx_diag(diag, "out of memory");
        if (fatal_out != NULL) {
            *fatal_out = true;
        }
        return NAN;
    }
    memcpy(scratch, text, len);
    scratch[len] = '\0';

    if (options->suffix != NULL) {
        size_t suffix_len = strlen(options->suffix);
        if (len >= suffix_len && memcmp(scratch + len - suffix_len, options->suffix, suffix_len) == 0) {
            scratch[len - suffix_len] = '\0';
        }
    }

    char* end = NULL;
    double value = 0.0;
    if (!bx_numfmt_parse_float_value(scratch, &value, &end)) {
        bx_diag(diag, "invalid number: '%s'", scratch);
        free(scratch);
        return NAN;
    }

    if (*end != '\0' && !bx_numfmt_parse_suffix(end, options->from_unit, &value)) {
        bx_diag(diag, "invalid number: '%s'", scratch);
        free(scratch);
        return NAN;
    }

    free(scratch);
    *ok_out = true;
    return value;
}

static const char* bx_numfmt_output_unit_label(enum unit_type to_unit, unsigned int power) {
    if (power > 8u) {
        power = 8u;
    }
    enum bx_size_unit_label_style style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;

    switch (to_unit) {
        case UNIT_SI:
            style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
            break;
        case UNIT_IEC:
            style = BX_SIZE_UNIT_LABEL_IEC_PREFIX;
            break;
        case UNIT_IEC_I:
            style = BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX;
            break;
        case UNIT_AUTO:
            style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
            break;
        case UNIT_NONE:
        default:
            return "";
    }

    const char* label = bx_size_unit_label(style, power);
    return label != NULL ? label : "";
}

static char* bx_numfmt_format_number(double value, const struct bx_numfmt_options* options) {
    char number_buf[256];
    double scaled = value;
    unsigned int power = 0u;

    if (options->to_unit != UNIT_NONE) {
        enum bx_size_unit_label_style style = BX_SIZE_UNIT_LABEL_SI_LOWER_K;
        if (options->to_unit == UNIT_IEC) {
            style = BX_SIZE_UNIT_LABEL_IEC_PREFIX;
        }
        else if (options->to_unit == UNIT_IEC_I) {
            style = BX_SIZE_UNIT_LABEL_IEC_I_SUFFIX;
        }

        double base = 0.0;
        if (!bx_size_unit_label_base_double(style, &base)) {
            scaled = value;
            power = 0u;
        }
        else if (!bx_size_scale_magnitude_double(value, base, base, 8u, &scaled, &power)) {
            scaled = value;
            power = 0u;
        }
    }

    const char* format = options->format;
    if (format == NULL) {
        if (options->to_unit == UNIT_NONE) {
            format = options->grouping ? "%'.0f" : "%.0f";
        }
        else {
            format = options->grouping ? "%'.1f" : "%.1f";
        }
    }

    snprintf(number_buf, sizeof(number_buf), format, scaled);

    const char* unit = bx_numfmt_output_unit_label(options->to_unit, power);
    size_t number_len = strlen(number_buf);
    size_t unit_len = strlen(unit);
    size_t suffix_len = (options->suffix != NULL) ? strlen(options->suffix) : 0;
    if (number_len > SIZE_MAX - unit_len
        || number_len + unit_len > SIZE_MAX - suffix_len
        || number_len + unit_len + suffix_len > SIZE_MAX - 1u) {
        return NULL;
    }
    size_t total_len = number_len + unit_len + suffix_len;

    char* unpadded = malloc(total_len + 1);
    if (unpadded == NULL) {
        return NULL;
    }
    strcpy(unpadded, number_buf);
    strcat(unpadded, unit);
    if (options->suffix != NULL) {
        strcat(unpadded, options->suffix);
    }

    size_t width = strlen(unpadded);
    size_t target_width = width;
    size_t abs_padding = 0u;
    if (options->padding < 0) {
        abs_padding = options->padding == INT_MIN
            ? (size_t)INT_MAX + 1u
            : (size_t)(-options->padding);
    }
    else {
        abs_padding = (size_t)options->padding;
    }
    if (abs_padding > 0 && abs_padding > target_width) {
        target_width = abs_padding;
    }

    if (target_width > SIZE_MAX - 1u) {
        free(unpadded);
        return NULL;
    }

    char* padded = malloc(target_width + 1);
    if (padded == NULL) {
        free(unpadded);
        return NULL;
    }
    if (abs_padding <= width || options->padding == 0) {
        memcpy(padded, unpadded, width + 1);
    }
    else if (options->padding > 0) {
        size_t pad = target_width - width;
        memset(padded, ' ', pad);
        memcpy(padded + pad, unpadded, width + 1);
    }
    else {
        memcpy(padded, unpadded, width);
        memset(padded + width, ' ', target_width - width);
        padded[target_width] = '\0';
    }

    free(unpadded);
    return padded;
}

static bool bx_numfmt_process_direct_number(
    const char* text,
    size_t len,
    const struct bx_numfmt_options* options,
    struct bx_diag_ctx* diag
) {
    bool ok = false;
    bool fatal = false;
    double value = bx_numfmt_parse_number_text(text, len, options, diag, &ok, &fatal);
    if (!ok) {
        return !fatal;
    }

    char* formatted = bx_numfmt_format_number(value, options);
    if (formatted == NULL) {
        bx_diag(diag, "out of memory");
        return false;
    }
    puts(formatted);
    free(formatted);
    return true;
}

static bool bx_numfmt_process_line(char* line, const struct bx_numfmt_options* options, struct bx_diag_ctx* diag) {
    size_t len = strlen(line);
    bool has_newline = (len > 0 && line[len - 1] == '\n');
    size_t content_len = has_newline ? (len - 1) : len;

    if (options->field == 1 && options->delimiter == '\0') {
        bool ok = false;
        bool fatal = false;
        double value = bx_numfmt_parse_number_text(line, content_len, options, diag, &ok, &fatal);
        if (!ok) {
            if (fatal) {
                return false;
            }
            fputs(line, stdout);
            return true;
        }

        char* formatted = bx_numfmt_format_number(value, options);
        if (formatted == NULL) {
            bx_diag(diag, "out of memory");
            return false;
        }
        fputs(formatted, stdout);
        if (has_newline) {
            fputc('\n', stdout);
        }
        free(formatted);
        return true;
    }

    const char* field_start = NULL;
    const char* field_end = NULL;

    if (options->delimiter != '\0') {
        const char* p = line;
        int current_field = 1;
        field_start = p;
        while (current_field < options->field) {
            const char* delim = strchr(field_start, options->delimiter);
            if (delim == NULL || (has_newline && delim >= line + content_len)) {
                fputs(line, stdout);
                return true;
            }
            field_start = delim + 1;
            current_field++;
        }

        field_end = field_start;
        while (*field_end != '\0' && *field_end != '\n' && *field_end != options->delimiter) {
            field_end++;
        }
    }
    else {
        const char* p = line;
        int current_field = 0;
        while (*p != '\0' && *p != '\n') {
            while (bx_numfmt_is_field_space((unsigned char)*p)) {
                p++;
            }
            if (*p == '\0' || *p == '\n') {
                break;
            }

            current_field++;
            const char* token_start = p;
            while (*p != '\0' && *p != '\n' && !bx_numfmt_is_field_space((unsigned char)*p)) {
                p++;
            }

            if (current_field == options->field) {
                field_start = token_start;
                field_end = p;
                break;
            }
        }
    }

    if (field_start == NULL || field_end == NULL) {
        fputs(line, stdout);
        return true;
    }

    bool ok = false;
    bool fatal = false;
    double value = bx_numfmt_parse_number_text(field_start, (size_t)(field_end - field_start), options, diag, &ok, &fatal);
    if (!ok) {
        if (fatal) {
            return false;
        }
        fputs(line, stdout);
        return true;
    }

    char* formatted = bx_numfmt_format_number(value, options);
    if (formatted == NULL) {
        bx_diag(diag, "out of memory");
        return false;
    }
    fwrite(line, 1, (size_t)(field_start - line), stdout);
    fputs(formatted, stdout);
    fputs(field_end, stdout);
    free(formatted);
    return true;
}

int bx_numfmt_main(int argc, char** argv) {
    struct bx_numfmt_options options;
    struct bx_diag_ctx diag = {.progname = "numfmt", .exit_status = 0};
    int first_operand = 0;

    setlocale(LC_ALL, "");

    enum bx_numfmt_parse_status parse_status =
        bx_numfmt_parse_options(argc, argv, &options, &first_operand, &diag);
    if (parse_status != BX_NUMFMT_PARSE_OK) {
        if (parse_status == BX_NUMFMT_PARSE_ERROR_TRY_HELP) {
            bx_cli_print_try_help(options.progname);
        }
        return diag.exit_status;
    }

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
            if (!bx_numfmt_process_direct_number(argv[first_operand + i], strlen(argv[first_operand + i]), &options, &diag)) {
                return diag.exit_status;
            }
        }
        return diag.exit_status;
    }

    char* line = NULL;
    size_t cap = 0;
    int header_count = 0;
    int read_errno = 0;
    for (;;) {
        errno = 0;
        ssize_t line_len = getline(&line, &cap, stdin);
        if (line_len < 0) {
            if (errno != 0) {
                read_errno = errno;
            }
            else if (ferror(stdin)) {
                read_errno = errno != 0 ? errno : EIO;
            }
            break;
        }
        if (header_count < options.header) {
            fputs(line, stdout);
            header_count++;
            continue;
        }
        if (!bx_numfmt_process_line(line, &options, &diag)) {
            free(line);
            return diag.exit_status;
        }
    }
    free(line);
    if (read_errno != 0) {
        bx_diag(&diag, "standard input: %s", strerror(read_errno));
    }

    return diag.exit_status;
}
