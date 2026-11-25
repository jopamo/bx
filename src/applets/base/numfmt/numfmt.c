#include <ctype.h>
#include <errno.h>
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
#include "lib/option_scan.h"

enum { BX_NUMFMT_NUMBER_SIZE = 256 };

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
    const char** numbers;
    size_t number_count;
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
    fprintf(stream, "  --format=F         use one printf-style floating-point conversion\n");
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

static bool bx_numfmt_format_size(const char** cursor) {
    unsigned size = 0;
    while (**cursor >= '0' && **cursor <= '9') {
        size = size * 10 + (unsigned)(**cursor - '0');
        if (size >= BX_NUMFMT_NUMBER_SIZE)
            return false;
        (*cursor)++;
    }
    return true;
}

/* snprintf receives exactly one double, with no auxiliary arguments. */
static bool bx_numfmt_valid_format(const char* format) {
    if (strnlen(format, BX_NUMFMT_NUMBER_SIZE) >= BX_NUMFMT_NUMBER_SIZE)
        return false;

    bool conversion = false;
    for (const char* p = format; *p; p++) {
        if (*p != '%')
            continue;
        if (*++p == '%')
            continue;
        if (conversion)
            return false;
        while (*p && strchr("-+ #0'", *p))
            p++;
        if (!bx_numfmt_format_size(&p))
            return false;
        if (*p == '.') {
            p++;
            if (!bx_numfmt_format_size(&p))
                return false;
        }
        if (*p == 'l')
            p++;
        if (!*p || !strchr("aAeEfFgG", *p))
            return false;
        conversion = true;
    }
    return conversion;
}

static enum bx_numfmt_parse_status bx_numfmt_parse_options(
    int argc,
    char** argv,
    struct bx_numfmt_options* options,
    struct bx_diag_ctx* diag
) {
    static const struct bx_option_spec specs[] = {
        {1, 0, "from", BX_OPTION_REQUIRED_VALUE},  {2, 0, "to", BX_OPTION_REQUIRED_VALUE},        {3, 0, "padding", BX_OPTION_REQUIRED_VALUE}, {4, 0, "header", BX_OPTION_OPTIONAL_VALUE},
        {5, 0, "field", BX_OPTION_REQUIRED_VALUE}, {6, 0, "delimiter", BX_OPTION_REQUIRED_VALUE}, {7, 0, "suffix", BX_OPTION_REQUIRED_VALUE},  {8, 0, "format", BX_OPTION_REQUIRED_VALUE},
        {9, 0, "grouping", BX_OPTION_NO_VALUE},    {10, 0, "help", BX_OPTION_NO_VALUE},           {11, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "numfmt");
    options->field = 1;
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!options->numbers) {
                options->numbers = calloc((size_t)argc - option.index, sizeof(*options->numbers));
                if (!options->numbers) {
                    bx_perror_path(diag, "allocate number operands");
                    return BX_NUMFMT_PARSE_ERROR;
                }
            }
            options->numbers[options->number_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option requires an argument -- '--%s'", option.spec->long_name);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return BX_NUMFMT_PARSE_ERROR_TRY_HELP;
        }
        const char* value = option.value;
        switch (option.spec->id) {
            case 1:
                if (!bx_numfmt_parse_unit(value, &options->from_unit, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 2:
                if (!bx_numfmt_parse_unit(value, &options->to_unit, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 3:
                if (!bx_numfmt_parse_padding(value, &options->padding, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 4:
                if (value == NULL) {
                    options->header = 1;
                    break;
                }
                if (!bx_numfmt_parse_header_count(value, &options->header, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 5:
                if (!bx_numfmt_parse_field_index(value, &options->field, diag)) {
                    return BX_NUMFMT_PARSE_ERROR;
                }
                break;
            case 6:
                if (value[0] == '\0' || value[1] != '\0') {
                    bx_diag(diag, "delimiter must be a single character: '%s'", value);
                    return BX_NUMFMT_PARSE_ERROR;
                }
                options->delimiter = value[0];
                break;
            case 7:
                options->suffix = value;
                break;
            case 8:
                if (!bx_numfmt_valid_format(value)) {
                    bx_diag(diag, "invalid floating-point format");
                    return BX_NUMFMT_PARSE_ERROR;
                }
                options->format = value;
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
            default:
                return BX_NUMFMT_PARSE_ERROR;
        }
    }

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
    char number_buf[BX_NUMFMT_NUMBER_SIZE];
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

    errno = 0;
    int len = snprintf(number_buf, sizeof(number_buf), format, scaled);
    if (len < 0) {
        if (errno == 0)
            errno = EIO;
        return NULL;
    }
    if ((size_t)len >= sizeof(number_buf)) {
        errno = EOVERFLOW;
        return NULL;
    }

    const char* unit = bx_numfmt_output_unit_label(options->to_unit, power);
    size_t number_len = (size_t)len;
    size_t unit_len = strlen(unit);
    size_t suffix_len = (options->suffix != NULL) ? strlen(options->suffix) : 0;
    if (number_len > SIZE_MAX - unit_len
        || number_len + unit_len > SIZE_MAX - suffix_len
        || number_len + unit_len + suffix_len > SIZE_MAX - 1u) {
        errno = EOVERFLOW;
        return NULL;
    }
    size_t total_len = number_len + unit_len + suffix_len;

    char* unpadded = malloc(total_len + 1);
    if (unpadded == NULL) {
        errno = ENOMEM;
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
        errno = EOVERFLOW;
        return NULL;
    }

    char* padded = malloc(target_width + 1);
    if (padded == NULL) {
        free(unpadded);
        errno = ENOMEM;
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

static bool bx_numfmt_format_error(struct bx_diag_ctx* diag) {
    int err = errno;
    if (err == ENOMEM)
        bx_diag(diag, "out of memory");
    else
        bx_diag(diag, "cannot format number: %s", bx_strerror(err));
    return false;
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
    if (formatted == NULL)
        return bx_numfmt_format_error(diag);
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
        if (formatted == NULL)
            return bx_numfmt_format_error(diag);
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
    if (formatted == NULL)
        return bx_numfmt_format_error(diag);
    fwrite(line, 1, (size_t)(field_start - line), stdout);
    fputs(formatted, stdout);
    fputs(field_end, stdout);
    free(formatted);
    return true;
}

int bx_numfmt_main(int argc, char** argv) {
    struct bx_numfmt_options options;
    struct bx_diag_ctx diag = {.progname = "numfmt", .exit_status = 0};
    setlocale(LC_ALL, "");

    enum bx_numfmt_parse_status parse_status = bx_numfmt_parse_options(argc, argv, &options, &diag);
    if (parse_status != BX_NUMFMT_PARSE_OK) {
        if (parse_status == BX_NUMFMT_PARSE_ERROR_TRY_HELP) {
            bx_cli_print_try_help(options.progname);
        }
        diag.exit_status = 1;
        goto done;
    }

    if (options.show_help) {
        bx_numfmt_print_help(stdout, options.progname);
        goto done;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        goto done;
    }

    if (options.number_count) {
        for (size_t i = 0; i < options.number_count; i++) {
            const char* number = options.numbers[i];
            if (!bx_numfmt_process_direct_number(number, strlen(number), &options, &diag))
                break;
        }
        goto done;
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
            break;
        }
    }
    free(line);
    if (read_errno != 0) {
        bx_diag(&diag, "standard input: %s", bx_strerror(read_errno));
    }

done:
    free(options.numbers);
    return diag.exit_status;
}
