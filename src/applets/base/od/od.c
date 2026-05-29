#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "lib/xreadwrite.h"
#include "lib/args_common.h"

enum bx_od_endian_mode {
    BX_OD_ENDIAN_NATIVE = 0,
    BX_OD_ENDIAN_LITTLE,
    BX_OD_ENDIAN_BIG,
};

enum bx_od_format_kind {
    BX_OD_FMT_NAMED_CHAR = 0,
    BX_OD_FMT_CHAR,
    BX_OD_FMT_SIGNED_DECIMAL,
    BX_OD_FMT_UNSIGNED_DECIMAL,
    BX_OD_FMT_OCTAL,
    BX_OD_FMT_HEXADECIMAL,
    BX_OD_FMT_FLOAT,
};

enum bx_od_float_mode {
    BX_OD_FLOAT_HALF = 0,
    BX_OD_FLOAT_BFLOAT16,
    BX_OD_FLOAT_NATIVE,
};

enum bx_od_parse_result {
    BX_OD_PARSE_OK = 0,
    BX_OD_PARSE_INVALID,
    BX_OD_PARSE_TOO_LARGE,
};

struct bx_od_format {
    enum bx_od_format_kind kind;
    enum bx_od_float_mode float_mode;
    size_t unit_size;
    bool char_suffix;
    size_t intrinsic_width;
    size_t pad_width;
};

struct bx_od_options {
    const char* progname;
    const char* width_option_name;
    char address_radix;
    uintmax_t skip_bytes;
    uintmax_t read_bytes;
    bool read_bytes_set;
    bool output_duplicates;
    uintmax_t width;
    bool width_specified;
    bool strings_mode;
    uintmax_t strings_min;
    bool traditional_mode;
    bool show_help;
    bool show_version;
    bool saw_nontraditional_option;
    enum bx_od_endian_mode endian_mode;
    struct bx_od_format* formats;
    size_t format_count;
    size_t format_capacity;
    size_t width_per_block;
    bool width_warning_pending;
    uintmax_t width_warning_original;
};

struct bx_od_operands {
    const char** files;
    size_t file_count;
    uintmax_t offset;
    bool have_label;
    uintmax_t label;
};

struct bx_od_input {
    const char** files;
    size_t file_count;
    size_t next_index;
    int current_fd;
    const char* current_name;
    bool current_is_stdin;
    bool retried_current_stdin_error;
    bool opened_any;
    bool had_error;
    struct bx_diag_ctx* diag;
};

static bool bx_od_host_is_little_endian(void) {
    const uint16_t value = 1u;
    return *((const unsigned char*)&value) == 1u;
}
static char* bx_od_push_c_numeric_locale(void) {
    const char* current = setlocale(LC_NUMERIC, NULL);
    char* saved = NULL;

    if (current != NULL) {
        saved = xstrdup(current);
        setlocale(LC_NUMERIC, "C");
    }

    return saved;
}

static void bx_od_pop_numeric_locale(char* saved_locale) {
    if (saved_locale != NULL) {
        setlocale(LC_NUMERIC, saved_locale);
        free(saved_locale);
    }
}


static void bx_od_warn(const char* progname, const char* fmt, ...) {
    va_list ap;

    fprintf(stderr, "%s: warning: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void bx_od_maybe_warn_width_adjustment(const struct bx_od_options* options,
                                              const struct bx_od_input* input,
                                              bool* warned) {
    if (options == NULL || input == NULL || warned == NULL) {
        return;
    }

    if (*warned || !options->width_warning_pending || !input->opened_any) {
        return;
    }

    bx_od_warn(options->progname,
               "invalid width %ju; using %ju instead",
               options->width_warning_original,
               options->width);
    *warned = true;
}

static void bx_od_try_help(const struct bx_diag_ctx* diag) {
    fprintf(stderr, "Try '%s --help' for more information.\n", diag->progname);
}

static void bx_od_option_too_large(struct bx_diag_ctx* diag, const char* option_name, const char* arg) {
    fprintf(stderr, "%s: %s argument '%s' too large\n", diag->progname, option_name, arg ? arg : "");
    diag->exit_status = 1;
}

static void bx_od_invalid_option_short(struct bx_diag_ctx* diag, int opt) {
    fprintf(stderr, "%s: invalid option -- '%c'\n", diag->progname, opt);
    bx_od_try_help(diag);
    diag->exit_status = 1;
}

static void bx_od_unrecognized_option(struct bx_diag_ctx* diag, const char* arg) {
    if (arg != NULL) {
        fprintf(stderr, "%s: unrecognized option '%s'\n", diag->progname, arg);
    }
    else {
        fprintf(stderr, "%s: unrecognized option\n", diag->progname);
    }
    bx_od_try_help(diag);
    diag->exit_status = 1;
}

static void bx_od_invalid_suffix_in_argument(struct bx_diag_ctx* diag,
                                             const char* option_name,
                                             const char* arg) {
    fprintf(stderr, "%s: invalid suffix in %s argument '%s'\n", diag->progname, option_name, arg ? arg : "");
    diag->exit_status = 1;
}

static bool bx_od_parse_bytes_with_diag(const char* option_name,
                                        const char* text,
                                        uintmax_t* value_out,
                                        struct bx_diag_ctx* diag);

static void bx_od_traditional_extra_operand(struct bx_diag_ctx* diag, const char* operand) {
    fprintf(stderr, "%s: extra operand '%s'\n", diag->progname, operand);
    fprintf(stderr, "%s: compatibility mode supports at most one file\n", diag->progname);
    fprintf(stderr, "Try '%s --help' for more information.\n", diag->progname);
    diag->exit_status = 1;
}

static bool bx_od_short_option_requires_argument(int c) {
    return c == 'A' || c == 'j' || c == 'N' || c == 'S' || c == 't' || c == 'w';
}

static bool bx_od_safe_mul(uintmax_t a, uintmax_t b, uintmax_t* out) {
    if (out == NULL) {
        return false;
    }

    if (a != 0u && b > UINTMAX_MAX / a) {
        return false;
    }

    *out = a * b;
    return true;
}

static uintmax_t bx_od_gcd(uintmax_t a, uintmax_t b) {
    while (b != 0u) {
        uintmax_t t = a % b;
        a = b;
        b = t;
    }

    return a;
}

static bool bx_od_lcm(uintmax_t a, uintmax_t b, uintmax_t* out) {
    uintmax_t g;
    uintmax_t reduced;

    if (a == 0u || b == 0u || out == NULL) {
        return false;
    }

    g = bx_od_gcd(a, b);
    reduced = a / g;
    return bx_od_safe_mul(reduced, b, out);
}

static size_t bx_od_pad_at(size_t fields, size_t i, size_t pad) {
    size_t whole;
    size_t rem;

    if (fields == 0u) {
        return 0u;
    }

    whole = pad / fields;
    rem = pad % fields;
    return whole * i + (rem * i) / fields;
}

static void bx_od_print_spaces(size_t count) {
    while (count > 0u) {
        fputc(' ', stdout);
        count--;
    }
}

static enum bx_od_parse_result bx_od_parse_prefixed_uint(const char* text,
                                                         uintmax_t* value_out,
                                                         const char** end_out) {
    const char* p = text;
    uintmax_t value = 0u;
    int base = 10;
    bool have_digit = false;

    if (text == NULL || value_out == NULL) {
        return BX_OD_PARSE_INVALID;
    }

    if (*p == '+') {
        p++;
    }

    if (*p == '\0') {
        return BX_OD_PARSE_INVALID;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    }

    while (*p != '\0') {
        unsigned int digit;

        if (*p >= '0' && *p <= '9') {
            digit = (unsigned int)(*p - '0');
        }
        else if (base == 16 && *p >= 'a' && *p <= 'f') {
            digit = (unsigned int)(*p - 'a') + 10u;
        }
        else if (base == 16 && *p >= 'A' && *p <= 'F') {
            digit = (unsigned int)(*p - 'A') + 10u;
        }
        else {
            break;
        }

        if (digit >= (unsigned int)base) {
            break;
        }

        if (value > (UINTMAX_MAX - (uintmax_t)digit) / (uintmax_t)base) {
            return BX_OD_PARSE_TOO_LARGE;
        }

        value = value * (uintmax_t)base + (uintmax_t)digit;
        have_digit = true;
        p++;
    }

    if (!have_digit) {
        return BX_OD_PARSE_INVALID;
    }

    *value_out = value;
    if (end_out != NULL) {
        *end_out = p;
    }
    return BX_OD_PARSE_OK;
}

static enum bx_od_parse_result bx_od_parse_byte_suffix(const char* suffix, uintmax_t* multiplier_out) {
    char canonical[4];
    char prefix;
    enum bx_size_suffix_parse_result status;

    if (suffix == NULL || multiplier_out == NULL) {
        return BX_OD_PARSE_INVALID;
    }

    if (suffix[0] == '\0' || strcmp(suffix, "b") == 0) {
        status = bx_size_suffix_multiplier_result(suffix, multiplier_out);
    }
    else {
        prefix = suffix[0] == 'k' ? 'K' : suffix[0];
        if (strchr("KMGTPEZYRQ", prefix) == NULL) {
            return BX_OD_PARSE_INVALID;
        }

        canonical[0] = prefix;
        if (suffix[1] == '\0') {
            canonical[1] = '\0';
        }
        else if ((suffix[1] == 'B' || suffix[1] == 'b') && suffix[2] == '\0') {
            canonical[1] = 'B';
            canonical[2] = '\0';
        }
        else if ((suffix[1] == 'i' || suffix[1] == 'I') && (suffix[2] == 'B' || suffix[2] == 'b') && suffix[3] == '\0') {
            canonical[1] = 'i';
            canonical[2] = 'B';
            canonical[3] = '\0';
        }
        else {
            return BX_OD_PARSE_INVALID;
        }

        status = bx_size_suffix_multiplier_result(canonical, multiplier_out);
    }

    if (status == BX_SIZE_SUFFIX_PARSE_OK) {
        return BX_OD_PARSE_OK;
    }
    return status == BX_SIZE_SUFFIX_PARSE_TOO_LARGE ? BX_OD_PARSE_TOO_LARGE : BX_OD_PARSE_INVALID;
}

static enum bx_od_parse_result bx_od_parse_bytes(const char* text, uintmax_t* value_out) {
    uintmax_t value;
    uintmax_t multiplier;
    const char* suffix;
    enum bx_od_parse_result status;

    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return BX_OD_PARSE_INVALID;
    }

    if (text[0] == '-') {
        return BX_OD_PARSE_INVALID;
    }

    status = bx_od_parse_prefixed_uint(text, &value, &suffix);
    if (status != BX_OD_PARSE_OK) {
        return status;
    }

    status = bx_od_parse_byte_suffix(suffix, &multiplier);
    if (status != BX_OD_PARSE_OK) {
        return status;
    }

    if (!bx_od_safe_mul(value, multiplier, value_out)) {
        return BX_OD_PARSE_TOO_LARGE;
    }

    return BX_OD_PARSE_OK;
}

static bool bx_od_parse_bytes_with_diag(const char* option_name,
                                        const char* text,
                                        uintmax_t* value_out,
                                        struct bx_diag_ctx* diag) {
    enum bx_od_parse_result status = bx_od_parse_bytes(text, value_out);

    if (status == BX_OD_PARSE_OK) {
        return true;
    }

    if (status == BX_OD_PARSE_TOO_LARGE) {
        bx_od_option_too_large(diag, option_name, text);
    }
    else {
        bx_diag(diag, "invalid %s argument '%s'", option_name, text ? text : "");
    }

    return false;
}

static enum bx_od_parse_result bx_od_parse_width(const char* text, uintmax_t* value_out, bool* invalid_suffix_out) {
    const char* p = text;
    uintmax_t value = 0u;
    bool have_digit = false;

    if (invalid_suffix_out != NULL) {
        *invalid_suffix_out = false;
    }

    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return BX_OD_PARSE_INVALID;
    }

    if (*p == '+') {
        p++;
    }
    else if (*p == '-') {
        return BX_OD_PARSE_INVALID;
    }

    while (*p >= '0' && *p <= '9') {
        unsigned int digit = (unsigned int)(*p - '0');

        if (value > (UINTMAX_MAX - (uintmax_t)digit) / 10u) {
            return BX_OD_PARSE_TOO_LARGE;
        }

        value = value * 10u + (uintmax_t)digit;
        have_digit = true;
        p++;
    }

    if (!have_digit) {
        return BX_OD_PARSE_INVALID;
    }

    if (*p != '\0') {
        if (invalid_suffix_out != NULL) {
            *invalid_suffix_out = true;
        }
        return BX_OD_PARSE_INVALID;
    }

    *value_out = value;
    return BX_OD_PARSE_OK;
}

static bool bx_od_parse_endian_value(const char* text, enum bx_od_endian_mode* value_out, bool* ambiguous_out) {
    size_t len;
    bool matches_big;
    bool matches_little;

    if (ambiguous_out != NULL) {
        *ambiguous_out = false;
    }

    if (text == NULL || value_out == NULL) {
        return false;
    }

    len = strlen(text);
    matches_big = strncmp("big", text, len) == 0;
    matches_little = strncmp("little", text, len) == 0;

    if (matches_big && matches_little) {
        if (ambiguous_out != NULL) {
            *ambiguous_out = true;
        }
        return false;
    }

    if (matches_big) {
        *value_out = BX_OD_ENDIAN_BIG;
        return true;
    }

    if (matches_little) {
        *value_out = BX_OD_ENDIAN_LITTLE;
        return true;
    }

    return false;
}

static bool bx_od_parse_traditional_offset(const char* text, uintmax_t* value_out) {
    const char* p = text;
    size_t len;
    bool use_decimal = false;
    bool use_blocks = false;
    uintmax_t value = 0u;

    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    if (*p == '+') {
        p++;
    }

    if (*p == '\0') {
        return false;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        const char* end = NULL;

        if (bx_od_parse_prefixed_uint(p, &value, &end) != BX_OD_PARSE_OK) {
            return false;
        }

        if (end == NULL || *end != '\0') {
            return false;
        }

        *value_out = value;
        return true;
    }

    len = strlen(p);
    if (len == 0u) {
        return false;
    }

    if (p[len - 1u] == 'b') {
        use_blocks = true;
        len--;
    }

    if (len > 0u && p[len - 1u] == '.') {
        use_decimal = true;
        len--;
    }

    if (len == 0u) {
        return false;
    }

    if (use_decimal) {
        for (size_t i = 0u; i < len; i++) {
            unsigned int digit;

            if (p[i] < '0' || p[i] > '9') {
                return false;
            }

            digit = (unsigned int)(p[i] - '0');
            if (value > (UINTMAX_MAX - (uintmax_t)digit) / 10u) {
                return false;
            }
            value = value * 10u + (uintmax_t)digit;
        }
    }
    else {
        for (size_t i = 0u; i < len; i++) {
            unsigned int digit;

            if (p[i] < '0' || p[i] > '7') {
                return false;
            }

            digit = (unsigned int)(p[i] - '0');
            if (value > (UINTMAX_MAX - (uintmax_t)digit) / 8u) {
                return false;
            }
            value = value * 8u + (uintmax_t)digit;
        }
    }

    if (use_blocks && !bx_od_safe_mul(value, 512u, &value)) {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_od_parse_decimal_size(const char* text, size_t len, size_t* value_out) {
    size_t value = 0u;

    if (text == NULL || len == 0u || value_out == NULL) {
        return false;
    }

    for (size_t i = 0u; i < len; i++) {
        unsigned int digit;

        if (text[i] < '0' || text[i] > '9') {
            return false;
        }

        digit = (unsigned int)(text[i] - '0');
        if (value > (SIZE_MAX - (size_t)digit) / 10u) {
            return false;
        }

        value = value * 10u + (size_t)digit;
    }

    *value_out = value;
    return true;
}

static bool bx_od_integral_size_supported(size_t size) {
    return size == 1u || size == 2u || size == 4u || size == 8u;
}

static bool bx_od_float_size_supported(size_t size) {
    return size == 2u || size == sizeof(float) || size == sizeof(double) || size == sizeof(long double);
}

static bool bx_od_add_format(struct bx_od_options* options, const struct bx_od_format* format) {
    size_t new_capacity;

    if (options->format_count == options->format_capacity) {
        new_capacity = (options->format_capacity == 0u) ? 8u : options->format_capacity * 2u;
        options->formats = xrealloc(options->formats, new_capacity * sizeof(*options->formats));
        options->format_capacity = new_capacity;
    }

    options->formats[options->format_count++] = *format;
    return true;
}

static bool bx_od_append_format(struct bx_od_options* options,
                                enum bx_od_format_kind kind,
                                enum bx_od_float_mode float_mode,
                                size_t unit_size,
                                bool char_suffix) {
    struct bx_od_format format;

    memset(&format, 0, sizeof(format));
    format.kind = kind;
    format.float_mode = float_mode;
    format.unit_size = unit_size;
    format.char_suffix = char_suffix;

    return bx_od_add_format(options, &format);
}

static bool bx_od_append_named_shortcut(struct bx_od_options* options, int c) {
    switch (c) {
        case 'a':
            return bx_od_append_format(options, BX_OD_FMT_NAMED_CHAR, BX_OD_FLOAT_NATIVE, 1u, false);
        case 'b':
            return bx_od_append_format(options, BX_OD_FMT_OCTAL, BX_OD_FLOAT_NATIVE, 1u, false);
        case 'c':
            return bx_od_append_format(options, BX_OD_FMT_CHAR, BX_OD_FLOAT_NATIVE, 1u, false);
        case 'd':
            return bx_od_append_format(options, BX_OD_FMT_UNSIGNED_DECIMAL, BX_OD_FLOAT_NATIVE, 2u, false);
        case 'f':
            return bx_od_append_format(options, BX_OD_FMT_FLOAT, BX_OD_FLOAT_NATIVE, sizeof(float), false);
        case 'i':
            return bx_od_append_format(options, BX_OD_FMT_SIGNED_DECIMAL, BX_OD_FLOAT_NATIVE, sizeof(int), false);
        case 'l':
            return bx_od_append_format(options, BX_OD_FMT_SIGNED_DECIMAL, BX_OD_FLOAT_NATIVE, sizeof(long), false);
        case 'o':
            return bx_od_append_format(options, BX_OD_FMT_OCTAL, BX_OD_FLOAT_NATIVE, 2u, false);
        case 's':
            return bx_od_append_format(options, BX_OD_FMT_SIGNED_DECIMAL, BX_OD_FLOAT_NATIVE, 2u, false);
        case 'x':
            return bx_od_append_format(options, BX_OD_FMT_HEXADECIMAL, BX_OD_FLOAT_NATIVE, 2u, false);
        default:
            return false;
    }
}

static bool bx_od_parse_type_string(const char* text, struct bx_od_options* options, struct bx_diag_ctx* diag) {
    const char* p = text;

    if (text == NULL) {
        bx_diag(diag, "invalid type string ''");
        return false;
    }

    if (text[0] == '\0') {
        return true;
    }

    while (*p != '\0') {
        enum bx_od_format_kind kind;
        enum bx_od_float_mode float_mode = BX_OD_FLOAT_NATIVE;
        size_t unit_size = 0u;
        bool char_suffix = false;
        char type = *p++;

        switch (type) {
            case 'a':
                kind = BX_OD_FMT_NAMED_CHAR;
                unit_size = 1u;
                break;
            case 'c':
                kind = BX_OD_FMT_CHAR;
                unit_size = 1u;
                break;
            case 'd':
            case 'o':
            case 'u':
            case 'x': {
                const char* digits_start;
                size_t digits_len;

                if (type == 'd') {
                    kind = BX_OD_FMT_SIGNED_DECIMAL;
                }
                else if (type == 'o') {
                    kind = BX_OD_FMT_OCTAL;
                }
                else if (type == 'u') {
                    kind = BX_OD_FMT_UNSIGNED_DECIMAL;
                }
                else {
                    kind = BX_OD_FMT_HEXADECIMAL;
                }

                unit_size = sizeof(int);
                if (*p == 'C') {
                    unit_size = sizeof(char);
                    p++;
                }
                else if (*p == 'S') {
                    unit_size = sizeof(short);
                    p++;
                }
                else if (*p == 'I') {
                    unit_size = sizeof(int);
                    p++;
                }
                else if (*p == 'L') {
                    unit_size = sizeof(long);
                    p++;
                }
                else if (isdigit((unsigned char)*p)) {
                    digits_start = p;
                    digits_len = strspn(p, "0123456789");
                    if (!bx_od_parse_decimal_size(digits_start, digits_len, &unit_size)) {
                        bx_diag(diag, "invalid type string '%s'", text);
                        return false;
                    }
                    p += digits_len;
                }

                if (!bx_od_integral_size_supported(unit_size)) {
                    fprintf(stderr, "%s: invalid type string '%s';\n", diag->progname, text);
                    fprintf(stderr, "this system doesn't provide a %zu-byte integral type\n", unit_size);
                    diag->exit_status = 1;
                    return false;
                }
                break;
            }
            case 'f': {
                const char* digits_start;
                size_t digits_len;

                kind = BX_OD_FMT_FLOAT;
                unit_size = sizeof(double);
                if (*p == 'B') {
                    float_mode = BX_OD_FLOAT_BFLOAT16;
                    unit_size = 2u;
                    p++;
                }
                else if (*p == 'H') {
                    float_mode = BX_OD_FLOAT_HALF;
                    unit_size = 2u;
                    p++;
                }
                else if (*p == 'F') {
                    float_mode = BX_OD_FLOAT_NATIVE;
                    unit_size = sizeof(float);
                    p++;
                }
                else if (*p == 'D') {
                    float_mode = BX_OD_FLOAT_NATIVE;
                    unit_size = sizeof(double);
                    p++;
                }
                else if (*p == 'L') {
                    float_mode = BX_OD_FLOAT_NATIVE;
                    unit_size = sizeof(long double);
                    p++;
                }
                else if (isdigit((unsigned char)*p)) {
                    digits_start = p;
                    digits_len = strspn(p, "0123456789");
                    if (!bx_od_parse_decimal_size(digits_start, digits_len, &unit_size)) {
                        bx_diag(diag, "invalid type string '%s'", text);
                        return false;
                    }
                    p += digits_len;
                    if (unit_size == 2u) {
                        float_mode = BX_OD_FLOAT_HALF;
                    }
                }

                if (!bx_od_float_size_supported(unit_size)) {
                    fprintf(stderr, "%s: invalid type string '%s';\n", diag->progname, text);
                    fprintf(stderr, "this system doesn't provide a %zu-byte floating point type\n", unit_size);
                    diag->exit_status = 1;
                    return false;
                }
                break;
            }
            default:
                bx_diag(diag, "invalid character '%c' in type string '%s'", type, text);
                return false;
        }

        if (*p == 'z') {
            char_suffix = true;
            p++;
        }

        if (!bx_od_append_format(options, kind, float_mode, unit_size, char_suffix)) {
            bx_diag(diag, "out of memory");
            return false;
        }
    }

    return true;
}

static size_t bx_od_unsigned_digits(size_t unit_size) {
    switch (unit_size) {
        case 1u:
            return 3u;
        case 2u:
            return 5u;
        case 4u:
            return 10u;
        case 8u:
            return 20u;
        default:
            return 20u;
    }
}

static size_t bx_od_signed_digits(size_t unit_size) {
    switch (unit_size) {
        case 1u:
            return 4u;
        case 2u:
            return 6u;
        case 4u:
            return 11u;
        case 8u:
            return 20u;
        default:
            return 20u;
    }
}

static size_t bx_od_float_digits(size_t unit_size) {
    if (unit_size <= 4u) {
        return 15u;
    }

    if (unit_size == 8u) {
        return 24u;
    }

    return 29u;
}

static size_t bx_od_intrinsic_width(const struct bx_od_format* format) {
    switch (format->kind) {
        case BX_OD_FMT_NAMED_CHAR:
        case BX_OD_FMT_CHAR:
            return 3u;
        case BX_OD_FMT_SIGNED_DECIMAL:
            return bx_od_signed_digits(format->unit_size);
        case BX_OD_FMT_UNSIGNED_DECIMAL:
            return bx_od_unsigned_digits(format->unit_size);
        case BX_OD_FMT_OCTAL:
            return (format->unit_size * 8u + 2u) / 3u;
        case BX_OD_FMT_HEXADECIMAL:
            return format->unit_size * 2u;
        case BX_OD_FMT_FLOAT:
            return bx_od_float_digits(format->unit_size);
    }

    return 3u;
}

static bool bx_od_finalize_formats(struct bx_od_options* options, struct bx_diag_ctx* diag) {
    uintmax_t width_multiple = 1u;
    uintmax_t adjusted_width;

    if (options->strings_mode) {
        return true;
    }

    if (options->width > SIZE_MAX) {
        bx_diag(diag, "invalid %s argument '%ju'", options->width_option_name, options->width);
        return false;
    }

    for (size_t i = 0u; i < options->format_count; i++) {
        options->formats[i].intrinsic_width = bx_od_intrinsic_width(&options->formats[i]);
        options->formats[i].pad_width = 0u;
    }

    options->width_per_block = 0u;

    for (size_t i = 0u; i < options->format_count; i++) {
        if (!bx_od_lcm(width_multiple, (uintmax_t)options->formats[i].unit_size, &width_multiple)) {
            bx_diag(diag, "invalid line width");
            return false;
        }
    }

    if (!options->width_specified) {
        if (options->width < width_multiple) {
            options->width = width_multiple;
        }
        else if (width_multiple > 0u) {
            options->width -= options->width % width_multiple;
        }
    }
    else if (options->width == 0u) {
        bx_diag(diag, "invalid %s argument '%ju'", options->width_option_name, options->width);
        return false;
    }
    else {
        adjusted_width = options->width;
        if (adjusted_width < width_multiple || adjusted_width % width_multiple != 0u) {
            adjusted_width = width_multiple;
        }

        if (adjusted_width != options->width) {
            options->width_warning_pending = true;
            options->width_warning_original = options->width;
            options->width = adjusted_width;
        }
    }

    for (size_t i = 0u; i < options->format_count; i++) {
        size_t fields_per_block = (size_t)(options->width / options->formats[i].unit_size);
        size_t block_width;

        if (fields_per_block == 0u) {
            bx_diag(diag, "invalid line width");
            return false;
        }

        if (fields_per_block > 1u && fields_per_block - 1u > SIZE_MAX / (fields_per_block - 1u)) {
            bx_diag(diag, "invalid line width");
            return false;
        }

        if (options->formats[i].intrinsic_width > SIZE_MAX - 1u ||
            fields_per_block > SIZE_MAX / (options->formats[i].intrinsic_width + 1u)) {
            bx_diag(diag, "invalid line width");
            return false;
        }

        block_width = (options->formats[i].intrinsic_width + 1u) * fields_per_block;
        if (block_width > options->width_per_block) {
            options->width_per_block = block_width;
        }
    }

    for (size_t i = 0u; i < options->format_count; i++) {
        size_t fields_per_block = (size_t)(options->width / options->formats[i].unit_size);
        size_t block_width;

        if (fields_per_block > 0u && options->formats[i].intrinsic_width > SIZE_MAX / fields_per_block) {
            bx_diag(diag, "invalid line width");
            return false;
        }

        block_width = options->formats[i].intrinsic_width * fields_per_block;
        options->formats[i].pad_width = options->width_per_block - block_width;
    }

    return true;
}

static void bx_od_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "  or:  %s [-abcdfilosx]... [FILE] [[+]OFFSET[.][b]]\n", progname);
    fprintf(stream, "  or:  %s --traditional [OPTION]... [FILE] [[+]OFFSET[.][b] [+][LABEL][.][b]]\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Write an unambiguous representation, octal bytes by default,\n");
    fprintf(stream, "of FILE to standard output.  With more than one FILE argument,\n");
    fprintf(stream, "concatenate them in the listed order to form the input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "If first and second call formats both apply, the second format is assumed\n");
    fprintf(stream, "if the last operand begins with + or (if there are 2 operands) a digit.\n");
    fprintf(stream, "An OFFSET operand means -j OFFSET.  LABEL is the pseudo-address\n");
    fprintf(stream, "at first byte printed, incremented when dump is progressing.\n");
    fprintf(stream, "For OFFSET and LABEL, a 0x or 0X prefix indicates hexadecimal;\n");
    fprintf(stream, "suffixes may be . for decimal and b for multiply by 512.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -A, --address-radix=RADIX\n");
    fprintf(stream, "         output format for file offsets;\n");
    fprintf(stream, "         RADIX is one of [doxn], for Decimal, Octal, Hex or None\n");
    fprintf(stream, "      --endian={big|little}\n");
    fprintf(stream, "         swap input bytes according the specified order\n");
    fprintf(stream, "  -j, --skip-bytes=BYTES\n");
    fprintf(stream, "         skip BYTES input bytes first\n");
    fprintf(stream, "  -N, --read-bytes=BYTES\n");
    fprintf(stream, "         limit dump to BYTES input bytes\n");
    fprintf(stream, "  -S BYTES, --strings[=BYTES]\n");
    fprintf(stream, "         show only NUL terminated strings\n");
    fprintf(stream, "         of at least BYTES (default 3) printable characters\n");
    fprintf(stream, "  -t, --format=TYPE\n");
    fprintf(stream, "         select output format or formats\n");
    fprintf(stream, "  -v, --output-duplicates\n");
    fprintf(stream, "         do not use * to mark line suppression\n");
    fprintf(stream, "  -w[BYTES], --width[=BYTES]\n");
    fprintf(stream, "         output BYTES bytes per output line;\n");
    fprintf(stream, "         32 is implied when BYTES is not specified\n");
    fprintf(stream, "      --traditional\n");
    fprintf(stream, "         accept arguments in third form above\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "\n");
    fprintf(stream, "Traditional format specifications may be intermixed; they accumulate:\n");
    fprintf(stream, "  -a   same as -t a,  select named characters, ignoring high-order bit\n");
    fprintf(stream, "  -b   same as -t o1, select octal bytes\n");
    fprintf(stream, "  -c   same as -t c,  select printable characters or backslash escapes\n");
    fprintf(stream, "  -d   same as -t u2, select unsigned decimal 2-byte units\n");
    fprintf(stream, "  -f   same as -t fF, select floats\n");
    fprintf(stream, "  -i   same as -t dI, select decimal ints\n");
    fprintf(stream, "  -l   same as -t dL, select decimal longs\n");
    fprintf(stream, "  -o   same as -t o2, select octal 2-byte units\n");
    fprintf(stream, "  -s   same as -t d2, select decimal 2-byte units\n");
    fprintf(stream, "  -x   same as -t x2, select hexadecimal 2-byte units\n");
    fprintf(stream, "\n");
    fprintf(stream, "\n");
    fprintf(stream, "TYPE is made up of one or more of these specifications:\n");
    fprintf(stream, "  a          named character, ignoring high-order bit\n");
    fprintf(stream, "  c          printable character or backslash escape\n");
    fprintf(stream, "  d[SIZE]    signed decimal, SIZE bytes per integer\n");
    fprintf(stream, "  f[SIZE]    floating point, SIZE bytes per float\n");
    fprintf(stream, "  o[SIZE]    octal, SIZE bytes per integer\n");
    fprintf(stream, "  u[SIZE]    unsigned decimal, SIZE bytes per integer\n");
    fprintf(stream, "  x[SIZE]    hexadecimal, SIZE bytes per integer\n");
    fprintf(stream, "\n");
    fprintf(stream, "SIZE is a number.  For TYPE in [doux], SIZE may also be C for\n");
    fprintf(stream, "sizeof(char), S for sizeof(short), I for sizeof(int) or L for\n");
    fprintf(stream, "sizeof(long).  If TYPE is f, SIZE may also be B for Brain 16 bit,\n");
    fprintf(stream, "H for Half precision float, F for sizeof(float), D for sizeof(double),\n");
    fprintf(stream, "or L for sizeof(long double).\n");
    fprintf(stream, "\n");
    fprintf(stream, "Adding a z suffix to any type displays printable characters at the end of\n");
    fprintf(stream, "each output line.\n");
    fprintf(stream, "\n");
    fprintf(stream, "\n");
    fprintf(stream, "BYTES is hex with 0x or 0X prefix, and may have a multiplier suffix:\n");
    fprintf(stream, "  b    512\n");
    fprintf(stream, "  KB   1000\n");
    fprintf(stream, "  K    1024\n");
    fprintf(stream, "  MB   1000*1000\n");
    fprintf(stream, "  M    1024*1024\n");
    fprintf(stream, "and so on for G, T, P, E, Z, Y, R, Q.\n");
    fprintf(stream, "Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n");
}

static bool bx_od_parse_options(int argc, char** argv, struct bx_od_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    enum {
        BX_OD_OPT_HELP = 256,
        BX_OD_OPT_VERSION,
        BX_OD_OPT_ENDIAN,
        BX_OD_OPT_STRINGS,
        BX_OD_OPT_TRADITIONAL,
    };

    static const struct option long_options[] = {
        {"address-radix", required_argument, NULL, 'A'},
        {"endian", required_argument, NULL, BX_OD_OPT_ENDIAN},
        {"skip-bytes", required_argument, NULL, 'j'},
        {"read-bytes", required_argument, NULL, 'N'},
        {"strings", optional_argument, NULL, BX_OD_OPT_STRINGS},
        {"format", required_argument, NULL, 't'},
        {"output-duplicates", no_argument, NULL, 'v'},
        {"width", optional_argument, NULL, 'w'},
        {"traditional", no_argument, NULL, BX_OD_OPT_TRADITIONAL},
        {"help", no_argument, NULL, BX_OD_OPT_HELP},
        {"version", no_argument, NULL, BX_OD_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "od");
    options->width_option_name = "-w";
    options->address_radix = 'o';
    options->width = 16u;
    options->strings_min = 3u;
    options->endian_mode = BX_OD_ENDIAN_NATIVE;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "A:j:N:S:t:vw::abcdfilosx", long_options, &option_index);

        if (c == -1) {
            break;
        }

        switch (c) {
            case 'A':
                if (optarg == NULL || optarg[0] == '\0' || optarg[1] != '\0' ||
                    (optarg[0] != 'd' && optarg[0] != 'o' && optarg[0] != 'x' && optarg[0] != 'n')) {
                    bx_diag(diag, "invalid output address radix '%s'; it must be one character from [doxn]",
                            optarg ? optarg : "");
                    return false;
                }
                options->address_radix = optarg[0];
                options->saw_nontraditional_option = true;
                break;
            case BX_OD_OPT_ENDIAN:
                {
                    bool ambiguous = false;

                    if (bx_od_parse_endian_value(optarg, &options->endian_mode, &ambiguous)) {
                        options->saw_nontraditional_option = true;
                        break;
                    }

                    if (ambiguous) {
                        fprintf(stderr, "%s: ambiguous argument '%s' for '--endian'\n", diag->progname, optarg);
                    }
                    else {
                        fprintf(stderr, "%s: invalid argument '%s' for '--endian'\n", diag->progname, optarg);
                    }
                    fprintf(stderr, "Valid arguments are:\n");
                    fprintf(stderr, "  - 'little'\n");
                    fprintf(stderr, "  - 'big'\n");
                    bx_od_try_help(diag);
                    diag->exit_status = 1;
                    return false;
                }
            case 'j':
                if (!bx_od_parse_bytes_with_diag("-j", optarg, &options->skip_bytes, diag)) {
                    return false;
                }
                options->saw_nontraditional_option = true;
                break;
            case 'N':
                if (!bx_od_parse_bytes_with_diag("-N", optarg, &options->read_bytes, diag)) {
                    return false;
                }
                options->read_bytes_set = true;
                options->saw_nontraditional_option = true;
                break;
            case 'S':
                if (!bx_od_parse_bytes_with_diag("-S", optarg, &options->strings_min, diag)) {
                    return false;
                }
                options->strings_mode = true;
                options->saw_nontraditional_option = true;
                break;
            case BX_OD_OPT_STRINGS:
                options->strings_mode = true;
                if (optarg != NULL) {
                    if (!bx_od_parse_bytes_with_diag("--strings", optarg, &options->strings_min, diag)) {
                        return false;
                    }
                }
                options->saw_nontraditional_option = true;
                break;
            case 't':
                if (!bx_od_parse_type_string(optarg, options, diag)) {
                    return false;
                }
                options->saw_nontraditional_option = true;
                break;
            case 'v':
                options->output_duplicates = true;
                options->saw_nontraditional_option = true;
                break;
            case 'w':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL &&
                    strncmp(argv[optind - 1], "--width", 7u) == 0) {
                    options->width_option_name = "--width";
                }
                else {
                    options->width_option_name = "-w";
                }
                if (optarg == NULL) {
                    options->width = 32u;
                }
                else {
                    bool invalid_suffix = false;
                    enum bx_od_parse_result status = bx_od_parse_width(optarg, &options->width, &invalid_suffix);

                    if (status == BX_OD_PARSE_TOO_LARGE) {
                        bx_od_option_too_large(diag, options->width_option_name, optarg);
                        return false;
                    }
                    if (status != BX_OD_PARSE_OK) {
                        if (invalid_suffix) {
                            bx_od_invalid_suffix_in_argument(diag, options->width_option_name, optarg);
                        }
                        else {
                            bx_diag(diag, "invalid %s argument '%s'", options->width_option_name, optarg);
                        }
                        return false;
                    }
                }
                options->width_specified = true;
                options->saw_nontraditional_option = true;
                break;
            case 'a':
            case 'b':
            case 'c':
            case 'd':
            case 'f':
            case 'i':
            case 'l':
            case 'o':
            case 's':
            case 'x':
                if (!bx_od_append_named_shortcut(options, c)) {
                    bx_diag(diag, "out of memory");
                    return false;
                }
                break;
            case BX_OD_OPT_TRADITIONAL:
                options->traditional_mode = true;
                break;
            case BX_OD_OPT_HELP:
                options->show_help = true;
                *first_operand = optind;
                return true;
            case BX_OD_OPT_VERSION:
                options->show_version = true;
                *first_operand = optind;
                return true;
            case '?':
            default:
                if (bx_od_short_option_requires_argument(optopt)) {
                    fprintf(stderr, "%s: option requires an argument -- '%c'\n", diag->progname, optopt);
                    bx_od_try_help(diag);
                    diag->exit_status = 1;
                    return false;
                }
                if (optopt != 0 && isprint(optopt)) {
                    bx_od_invalid_option_short(diag, optopt);
                }
                else if (optind > 0 && optind <= argc) {
                    bx_od_unrecognized_option(diag, argv[optind - 1]);
                }
                else {
                    bx_od_unrecognized_option(diag, NULL);
                }
                return false;
        }
    }

    if (options->strings_mode && options->format_count != 0u) {
        bx_diag(diag, "no type may be specified when dumping strings");
        return false;
    }

    if (!options->strings_mode && options->format_count == 0u) {
        if (!bx_od_append_format(options, BX_OD_FMT_OCTAL, BX_OD_FLOAT_NATIVE, 2u, false)) {
            bx_diag(diag, "out of memory");
            return false;
        }
    }

    if (!bx_od_finalize_formats(options, diag)) {
        return false;
    }

    *first_operand = optind;
    return true;
}

static bool bx_od_is_legacy_offset_candidate(const char* text) {
    return text != NULL && (text[0] == '+' || isdigit((unsigned char)text[0]));
}

static bool bx_od_parse_operands(int argc,
                                 char** argv,
                                 int first_operand,
                                 const struct bx_od_options* options,
                                 struct bx_od_operands* operands,
                                 struct bx_diag_ctx* diag) {
    int count = argc - first_operand;
    const char** files = NULL;

    memset(operands, 0, sizeof(*operands));
    operands->offset = options->skip_bytes;

    if (count > 0) {
        files = xmalloc((size_t)count * sizeof(*files));
        for (int i = 0; i < count; i++) {
            files[i] = argv[first_operand + i];
        }
    }

    if (options->traditional_mode) {
        uintmax_t value1;
        uintmax_t value2;

        if (count == 0) {
            operands->files = files;
            return true;
        }

        if (count == 1) {
            if (bx_od_parse_traditional_offset(files[0], &value1)) {
                operands->files = files;
                operands->offset = value1;
                return true;
            }

            operands->files = files;
            operands->file_count = 1u;
            return true;
        }

        if (count == 2) {
            bool first_is_offset = bx_od_parse_traditional_offset(files[0], &value1);
            bool second_is_offset = bx_od_parse_traditional_offset(files[1], &value2);

            if (first_is_offset && second_is_offset) {
                operands->files = files;
                operands->offset = value1;
                operands->have_label = true;
                operands->label = value2;
                return true;
            }

            if (!first_is_offset && second_is_offset) {
                operands->files = files;
                operands->file_count = 1u;
                operands->offset = value2;
                return true;
            }

            bx_od_traditional_extra_operand(diag, files[1]);
            free((void*)files);
            return false;
        }

        if (count == 3) {
            if (!bx_od_parse_traditional_offset(files[1], &value1) || !bx_od_parse_traditional_offset(files[2], &value2)) {
                bx_od_traditional_extra_operand(diag, files[1]);
                free((void*)files);
                return false;
            }

            operands->files = files;
            operands->file_count = 1u;
            operands->offset = value1;
            operands->have_label = true;
            operands->label = value2;
            return true;
        }

        bx_od_traditional_extra_operand(diag, files[1]);
        free((void*)files);
        return false;
    }

    if (!options->saw_nontraditional_option && count <= 2) {
        uintmax_t value;

        if (count == 1 && files[0][0] == '+' && bx_od_parse_traditional_offset(files[0], &value)) {
            operands->files = files;
            operands->offset = value;
            return true;
        }

        if (count == 2 && bx_od_is_legacy_offset_candidate(files[1]) && bx_od_parse_traditional_offset(files[1], &value)) {
            operands->files = files;
            operands->file_count = 1u;
            operands->offset = value;
            return true;
        }
    }

    operands->files = files;
    operands->file_count = (size_t)((count > 0) ? count : 0);
    return true;
}

static void bx_od_input_init(struct bx_od_input* input,
                             const struct bx_od_operands* operands,
                             struct bx_diag_ctx* diag) {
    memset(input, 0, sizeof(*input));
    input->files = operands->files;
    input->file_count = operands->file_count;
    input->current_fd = -1;
    input->had_error = false;
    input->diag = diag;
}

static bool bx_od_input_opened_any(const struct bx_od_input* input) {
    return input != NULL && input->opened_any;
}

static bool bx_od_operands_are_stdin_only(const struct bx_od_operands* operands) {
    if (operands->file_count == 0u) {
        return true;
    }

    for (size_t i = 0u; i < operands->file_count; i++) {
        if (strcmp(operands->files[i], "-") != 0) {
            return false;
        }
    }

    return true;
}

static bool bx_od_input_open_next(struct bx_od_input* input) {
    while (true) {
        if (input->file_count == 0u && input->next_index == 0u) {
            input->current_fd = STDIN_FILENO;
            input->current_name = "-";
            input->current_is_stdin = true;
            input->retried_current_stdin_error = false;
            input->opened_any = true;
            input->next_index = 1u;
            return true;
        }

        if (input->next_index >= input->file_count) {
            return false;
        }

        input->current_name = input->files[input->next_index++];
        if (strcmp(input->current_name, "-") == 0) {
            input->current_fd = STDIN_FILENO;
            input->current_is_stdin = true;
            input->retried_current_stdin_error = false;
            input->opened_any = true;
            return true;
        }

        input->current_fd = open(input->current_name, O_RDONLY);
        if (input->current_fd >= 0) {
            input->current_is_stdin = false;
            input->retried_current_stdin_error = false;
            input->opened_any = true;
            return true;
        }

        input->had_error = true;
        bx_diag(input->diag, "%s: %s", input->current_name, strerror(errno));
    }
}

static void bx_od_input_close_current(struct bx_od_input* input) {
    if (input->current_fd >= 0 && !input->current_is_stdin) {
        close(input->current_fd);
    }

    input->current_fd = -1;
    input->current_name = NULL;
    input->current_is_stdin = false;
    input->retried_current_stdin_error = false;
}

static size_t bx_od_input_read(struct bx_od_input* input, unsigned char* buffer, size_t count) {
    size_t total = 0u;

    while (total < count) {
        ssize_t nread;

        if (input->current_fd < 0 && !bx_od_input_open_next(input)) {
            break;
        }

        nread = bx_xread(input->current_fd, buffer + total, count - total);
        if (nread > 0) {
            total += (size_t)nread;
            continue;
        }

        if (nread < 0) {
            if (input->current_is_stdin && !input->retried_current_stdin_error) {
                input->retried_current_stdin_error = true;
                continue;
            }
            input->had_error = true;
            bx_diag(input->diag, "%s: %s", input->current_name, strerror(errno));
        }

        bx_od_input_close_current(input);
    }

    return total;
}

static bool bx_od_input_had_error(const struct bx_od_input* input) {
    return input != NULL && input->had_error;
}

static void bx_od_input_prime_zero_read(struct bx_od_input* input, const struct bx_od_options* options) {
    if (input == NULL || options == NULL) {
        return;
    }

    if (!(options->read_bytes_set && options->read_bytes == 0u)) {
        return;
    }

    if (input->current_fd >= 0) {
        return;
    }

    (void)bx_od_input_open_next(input);
}

static bool bx_od_input_skip(struct bx_od_input* input, uintmax_t count, bool try_seek_stdin) {
    unsigned char discard[8192];

    if (count == 0u) {
        return true;
    }

    if (try_seek_stdin) {
        if (input->current_fd < 0 && !bx_od_input_open_next(input)) {
            return false;
        }

        if (input->current_fd >= 0 && input->current_is_stdin && count <= (uintmax_t)INT64_MAX) {
            if (lseek(input->current_fd, (off_t)count, SEEK_CUR) >= (off_t)0) {
                return true;
            }
        }
    }

    while (count > 0u) {
        size_t chunk = sizeof(discard);

        if (input->current_fd < 0 && !bx_od_input_open_next(input)) {
            return false;
        }

        if (input->current_fd >= 0 && !input->current_is_stdin) {
            struct stat st;

            if (fstat(input->current_fd, &st) == 0 && S_ISREG(st.st_mode)) {
                off_t current = lseek(input->current_fd, (off_t)0, SEEK_CUR);

                if (current >= (off_t)0) {
                    uintmax_t remaining = (st.st_size > current) ? (uintmax_t)((uintmax_t)st.st_size - (uintmax_t)current) : 0u;

                    if (remaining > 0u) {
                        uintmax_t step = (count < remaining) ? count : remaining;

                        if (step <= (uintmax_t)INT64_MAX &&
                            lseek(input->current_fd, (off_t)step, SEEK_CUR) == current + (off_t)step) {
                            count -= step;
                            if (count == 0u) {
                                return true;
                            }
                            if (step == remaining) {
                                bx_od_input_close_current(input);
                                continue;
                            }
                        }
                    }
                    else {
                        bx_od_input_close_current(input);
                        continue;
                    }
                }
            }
        }

        if (count < (uintmax_t)chunk) {
            chunk = (size_t)count;
        }

        size_t nread = bx_od_input_read(input, discard, chunk);
        if (nread == 0u) {
            return false;
        }

        count -= (uintmax_t)nread;
    }

    return true;
}

static void bx_od_format_address_component(uintmax_t value, char radix, char* buffer, size_t buffer_size) {
    char raw[128];
    size_t min_width = (radix == 'x') ? 6u : 7u;
    size_t raw_len;
    size_t zeros;
    size_t pos = 0u;

    if (radix == 'd') {
        snprintf(raw, sizeof(raw), "%" PRIuMAX, value);
    }
    else if (radix == 'x') {
        snprintf(raw, sizeof(raw), "%" PRIxMAX, value);
    }
    else {
        snprintf(raw, sizeof(raw), "%" PRIoMAX, value);
    }

    raw_len = strlen(raw);
    if (raw_len >= min_width) {
        snprintf(buffer, buffer_size, "%s", raw);
        return;
    }

    zeros = min_width - raw_len;
    while (pos < zeros && pos + 1u < buffer_size) {
        buffer[pos++] = '0';
    }

    if (pos < buffer_size) {
        snprintf(buffer + pos, buffer_size - pos, "%s", raw);
    }
}

static void bx_od_build_prefix(char* buffer,
                               size_t buffer_size,
                               char radix,
                               uintmax_t address,
                               bool have_label,
                               uintmax_t label) {
    char address_buf[128];
    char label_buf[128];

    buffer[0] = '\0';

    if (radix != 'n') {
        bx_od_format_address_component(address, radix, address_buf, sizeof(address_buf));
        snprintf(buffer, buffer_size, "%s", address_buf);
    }

    if (have_label) {
        bx_od_format_address_component(label, (radix == 'n') ? 'o' : radix, label_buf, sizeof(label_buf));
        if (radix != 'n') {
            size_t len = strlen(buffer);
            snprintf(buffer + len, (len < buffer_size) ? buffer_size - len : 0u, " (%s)", label_buf);
        }
        else {
            snprintf(buffer, buffer_size, "(%s)", label_buf);
        }
    }
}

static uintmax_t bx_od_unpack_uint(const unsigned char* bytes,
                                   size_t bytes_available,
                                   size_t unit_size,
                                   enum bx_od_endian_mode endian_mode) {
    enum bx_od_endian_mode effective = endian_mode;
    uintmax_t value = 0u;

    if (effective == BX_OD_ENDIAN_NATIVE) {
        effective = bx_od_host_is_little_endian() ? BX_OD_ENDIAN_LITTLE : BX_OD_ENDIAN_BIG;
    }

    if (effective == BX_OD_ENDIAN_LITTLE) {
        for (size_t i = 0u; i < unit_size; i++) {
            unsigned int byte = (i < bytes_available) ? bytes[i] : 0u;
            value |= ((uintmax_t)byte) << (i * 8u);
        }
    }
    else {
        for (size_t i = 0u; i < unit_size; i++) {
            unsigned int byte = (i < bytes_available) ? bytes[i] : 0u;
            value = (value << 8u) | (uintmax_t)byte;
        }
    }

    return value;
}

static intmax_t bx_od_unpack_int(const unsigned char* bytes,
                                 size_t bytes_available,
                                 size_t unit_size,
                                 enum bx_od_endian_mode endian_mode) {
    uintmax_t uvalue = bx_od_unpack_uint(bytes, bytes_available, unit_size, endian_mode);

    if (unit_size == 8u) {
        int64_t value = (int64_t)(uint64_t)uvalue;
        return (intmax_t)value;
    }

    if (unit_size == 4u) {
        int32_t value = (int32_t)(uint32_t)uvalue;
        return (intmax_t)value;
    }

    if (unit_size == 2u) {
        int16_t value = (int16_t)(uint16_t)uvalue;
        return (intmax_t)value;
    }

    return (intmax_t)(int8_t)(uint8_t)uvalue;
}

static void bx_od_prepare_host_bytes(unsigned char* out,
                                     size_t unit_size,
                                     const unsigned char* bytes,
                                     size_t bytes_available,
                                     enum bx_od_endian_mode endian_mode) {
    enum bx_od_endian_mode effective = endian_mode;
    bool host_little = bx_od_host_is_little_endian();
    bool same_order;

    if (effective == BX_OD_ENDIAN_NATIVE) {
        effective = host_little ? BX_OD_ENDIAN_LITTLE : BX_OD_ENDIAN_BIG;
    }

    same_order = (host_little && effective == BX_OD_ENDIAN_LITTLE) || (!host_little && effective == BX_OD_ENDIAN_BIG);
    memset(out, 0, unit_size);

    for (size_t i = 0u; i < bytes_available && i < unit_size; i++) {
        if (same_order) {
            out[i] = bytes[i];
        }
        else {
            out[unit_size - 1u - i] = bytes[i];
        }
    }
}

static double bx_od_half_to_double(uint16_t bits) {
    unsigned int sign = (bits >> 15) & 1u;
    unsigned int exponent = (bits >> 10) & 0x1fu;
    unsigned int fraction = bits & 0x3ffu;
    double value;

    if (exponent == 0u) {
        if (fraction == 0u) {
            value = 0.0;
        }
        else {
            value = ldexp((double)fraction, -24);
        }
    }
    else if (exponent == 0x1fu) {
        value = (fraction == 0u) ? INFINITY : NAN;
    }
    else {
        value = ldexp(1.0 + ((double)fraction / 1024.0), (int)exponent - 15);
    }

    return sign ? -value : value;
}

static bool bx_od_float_bits_equal(float a, float b) {
    uint32_t ua = 0u;
    uint32_t ub = 0u;

    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

static bool bx_od_double_bits_equal(double a, double b) {
    uint64_t ua = 0u;
    uint64_t ub = 0u;

    memcpy(&ua, &a, sizeof(ua));
    memcpy(&ub, &b, sizeof(ub));
    return ua == ub;
}

static bool bx_od_long_double_bits_equal(long double a, long double b) {
    if (isnan(a) || isnan(b)) {
        return false;
    }

    if (a == b) {
        return a != 0.0L || signbit(a) == signbit(b);
    }

    return false;
}

static bool bx_od_generated_float_candidate_matches(const char* candidate, float value) {
    char* end = NULL;
    float parsed;

    /*
     * This strto* use is intentionally scoped to internally generated
     * snprintf() candidates under the C numeric locale. It is a round-trip
     * oracle for shortest-format selection, not an external input parser.
     */
    parsed = strtof(candidate, &end);
    return end != NULL && *end == '\0' && bx_od_float_bits_equal(parsed, value);
}

static bool bx_od_generated_double_candidate_matches(const char* candidate, double value) {
    char* end = NULL;
    double parsed;

    /*
     * See bx_od_generated_float_candidate_matches(): generated output
     * candidates are parsed only to verify exact round-trip formatting.
     */
    parsed = strtod(candidate, &end);
    return end != NULL && *end == '\0' && bx_od_double_bits_equal(parsed, value);
}

static bool bx_od_generated_long_double_candidate_matches(const char* candidate, long double value) {
    char* end = NULL;
    long double parsed;

    /*
     * See bx_od_generated_float_candidate_matches(): this is an internal
     * generated-candidate round-trip check, not an input boundary.
     */
    parsed = strtold(candidate, &end);
    return end != NULL && *end == '\0' && bx_od_long_double_bits_equal(parsed, value);
}

static void bx_od_format_shortest_float(float value, char* out, size_t out_size) {
    char candidate[128];
    char* saved_locale = bx_od_push_c_numeric_locale();
#ifdef FLT_DECIMAL_DIG
    const int max_precision = FLT_DECIMAL_DIG;
#else
    const int max_precision = 9;
#endif
    float abs_value = signbit(value) ? -value : value;
    int min_precision = (abs_value < FLT_MIN) ? 1 : FLT_DIG;

    if (!isfinite(value)) {
        snprintf(out, out_size, "%g", (double)value);
        bx_od_pop_numeric_locale(saved_locale);
        return;
    }

    for (int precision = min_precision; precision <= max_precision; precision++) {
        snprintf(candidate, sizeof(candidate), "%.*g", precision, (double)value);
        if (bx_od_generated_float_candidate_matches(candidate, value)) {
            snprintf(out, out_size, "%s", candidate);
            bx_od_pop_numeric_locale(saved_locale);
            return;
        }
    }

    snprintf(out, out_size, "%.*g", max_precision, (double)value);
    bx_od_pop_numeric_locale(saved_locale);
}

static void bx_od_format_shortest_double(double value, char* out, size_t out_size) {
    char candidate[128];
    char* saved_locale = bx_od_push_c_numeric_locale();
#ifdef DBL_DECIMAL_DIG
    const int max_precision = DBL_DECIMAL_DIG;
#else
    const int max_precision = 17;
#endif
    double abs_value = signbit(value) ? -value : value;
    int min_precision = (abs_value < DBL_MIN) ? 1 : DBL_DIG;

    if (!isfinite(value)) {
        snprintf(out, out_size, "%g", value);
        bx_od_pop_numeric_locale(saved_locale);
        return;
    }

    for (int precision = min_precision; precision <= max_precision; precision++) {
        snprintf(candidate, sizeof(candidate), "%.*g", precision, value);
        if (bx_od_generated_double_candidate_matches(candidate, value)) {
            snprintf(out, out_size, "%s", candidate);
            bx_od_pop_numeric_locale(saved_locale);
            return;
        }
    }

    snprintf(out, out_size, "%.*g", max_precision, value);
    bx_od_pop_numeric_locale(saved_locale);
}

static void bx_od_format_shortest_long_double(long double value, char* out, size_t out_size) {
    char candidate[160];
    char* saved_locale = bx_od_push_c_numeric_locale();
#ifdef LDBL_DECIMAL_DIG
    const int max_precision = LDBL_DECIMAL_DIG;
#elif defined(DECIMAL_DIG)
    const int max_precision = DECIMAL_DIG;
#else
    const int max_precision = 21;
#endif
    long double abs_value = signbit(value) ? -value : value;
    int min_precision = (abs_value < LDBL_MIN) ? 1 : LDBL_DIG;

    if (isnan(value) || isinf(value)) {
        snprintf(out, out_size, "%Lg", value);
        bx_od_pop_numeric_locale(saved_locale);
        return;
    }

    for (int precision = min_precision; precision <= max_precision; precision++) {
        snprintf(candidate, sizeof(candidate), "%.*Lg", precision, value);
        if (bx_od_generated_long_double_candidate_matches(candidate, value)) {
            snprintf(out, out_size, "%s", candidate);
            bx_od_pop_numeric_locale(saved_locale);
            return;
        }
    }

    snprintf(out, out_size, "%.*Lg", max_precision, value);
    bx_od_pop_numeric_locale(saved_locale);
}

static void bx_od_render_named_char(unsigned char byte, char* out, size_t out_size) {
    static const char* const names[33] = {
        "nul", "soh", "stx", "etx", "eot", "enq", "ack", "bel",
        " bs", " ht", " nl", " vt", " ff", " cr", " so", " si",
        "dle", "dc1", "dc2", "dc3", "dc4", "nak", "syn", "etb",
        "can", " em", "sub", "esc", " fs", " gs", " rs", " us",
        " sp",
    };
    unsigned char value = (unsigned char)(byte & 0x7fu);

    if (value <= 32u) {
        snprintf(out, out_size, "%s", names[value]);
        return;
    }

    if (value == 127u) {
        snprintf(out, out_size, "del");
        return;
    }

    if (isprint(value)) {
        snprintf(out, out_size, "%c", value);
        return;
    }

    snprintf(out, out_size, "%03o", value);
}

static void bx_od_render_char(unsigned char byte, char* out, size_t out_size) {
    switch (byte) {
        case '\0':
            snprintf(out, out_size, "\\0");
            break;
        case '\a':
            snprintf(out, out_size, "\\a");
            break;
        case '\b':
            snprintf(out, out_size, "\\b");
            break;
        case '\t':
            snprintf(out, out_size, "\\t");
            break;
        case '\n':
            snprintf(out, out_size, "\\n");
            break;
        case '\v':
            snprintf(out, out_size, "\\v");
            break;
        case '\f':
            snprintf(out, out_size, "\\f");
            break;
        case '\r':
            snprintf(out, out_size, "\\r");
            break;
        default:
            if (isprint(byte)) {
                snprintf(out, out_size, "%c", byte);
            }
            else {
                snprintf(out, out_size, "%03o", byte);
            }
            break;
    }
}

static bool bx_od_render_token(const struct bx_od_format* format,
                               const unsigned char* bytes,
                               size_t bytes_available,
                               enum bx_od_endian_mode endian_mode,
                               char* out,
                               size_t out_size) {
    switch (format->kind) {
        case BX_OD_FMT_NAMED_CHAR:
            bx_od_render_named_char(bytes[0], out, out_size);
            return true;
        case BX_OD_FMT_CHAR:
            bx_od_render_char(bytes[0], out, out_size);
            return true;
        case BX_OD_FMT_SIGNED_DECIMAL:
            snprintf(out, out_size, "%" PRIdMAX,
                     bx_od_unpack_int(bytes, bytes_available, format->unit_size, endian_mode));
            return true;
        case BX_OD_FMT_UNSIGNED_DECIMAL:
            snprintf(out, out_size, "%" PRIuMAX,
                     bx_od_unpack_uint(bytes, bytes_available, format->unit_size, endian_mode));
            return true;
        case BX_OD_FMT_OCTAL:
            snprintf(out, out_size, "%0*" PRIoMAX, (int)format->intrinsic_width,
                     bx_od_unpack_uint(bytes, bytes_available, format->unit_size, endian_mode));
            return true;
        case BX_OD_FMT_HEXADECIMAL:
            snprintf(out, out_size, "%0*" PRIxMAX, (int)format->intrinsic_width,
                     bx_od_unpack_uint(bytes, bytes_available, format->unit_size, endian_mode));
            return true;
        case BX_OD_FMT_FLOAT: {
            unsigned char host_bytes[sizeof(long double)];

            bx_od_prepare_host_bytes(host_bytes, format->unit_size, bytes, bytes_available, endian_mode);

            if (format->float_mode == BX_OD_FLOAT_HALF) {
                uint16_t raw = 0u;
                float value;

                memcpy(&raw, host_bytes, sizeof(raw));
                value = (float)bx_od_half_to_double(raw);
                bx_od_format_shortest_float(value, out, out_size);
                return true;
            }

            if (format->float_mode == BX_OD_FLOAT_BFLOAT16) {
                uint16_t raw = 0u;
                uint32_t bits = 0u;
                float value = 0.0f;

                memcpy(&raw, host_bytes, sizeof(raw));
                bits = ((uint32_t)raw) << 16u;
                memcpy(&value, &bits, sizeof(value));
                bx_od_format_shortest_float(value, out, out_size);
                return true;
            }

            if (format->unit_size == sizeof(float)) {
                float value = 0.0f;
                memcpy(&value, host_bytes, sizeof(value));
                bx_od_format_shortest_float(value, out, out_size);
                return true;
            }

            if (format->unit_size == sizeof(double)) {
                double value = 0.0;
                memcpy(&value, host_bytes, sizeof(value));
                bx_od_format_shortest_double(value, out, out_size);
                return true;
            }

            if (format->unit_size == sizeof(long double)) {
                long double value = 0.0L;
                memcpy(&value, host_bytes, sizeof(value));
                bx_od_format_shortest_long_double(value, out, out_size);
                return true;
            }

            return false;
        }
    }

    return false;
}

static void bx_od_print_ascii_trailer(const unsigned char* buffer, size_t size) {
    fputs("  >", stdout);
    for (size_t i = 0u; i < size; i++) {
        unsigned char ch = buffer[i];
        fputc(isprint(ch) ? (int)ch : '.', stdout);
    }
    fputc('<', stdout);
}

static bool bx_od_print_block(const struct bx_od_options* options,
                              const unsigned char* buffer,
                              size_t size,
                              uintmax_t address,
                              bool have_label,
                              uintmax_t label) {
    char prefix[256];
    char token[256];
    size_t prefix_len;

    bx_od_build_prefix(prefix, sizeof(prefix), options->address_radix, address, have_label, label);
    prefix_len = strlen(prefix);

    for (size_t i = 0u; i < options->format_count; i++) {
        const struct bx_od_format* format = &options->formats[i];
        size_t fields = (size_t)(options->width / format->unit_size);
        size_t blank_fields = ((size_t)options->width - size) / format->unit_size;
        size_t field_index = 0u;
        size_t pad_remaining = format->pad_width;

        if (i == 0u) {
            fputs(prefix, stdout);
        }
        else {
            bx_od_print_spaces(prefix_len);
        }

        for (size_t field = fields; field > blank_fields; field--) {
            size_t pos = field_index * format->unit_size;
            size_t bytes_available = format->unit_size;
            size_t next_pad;
            size_t adjusted_width;

            if (bytes_available > size - pos) {
                bytes_available = size - pos;
            }

            if (!bx_od_render_token(format, buffer + pos, bytes_available, options->endian_mode, token, sizeof(token))) {
                return false;
            }

            next_pad = bx_od_pad_at(fields, field - 1u, format->pad_width);
            adjusted_width = (pad_remaining - next_pad) + format->intrinsic_width;
            if (adjusted_width > (size_t)INT_MAX) {
                return false;
            }

            printf("%*s", (int)adjusted_width, token);
            pad_remaining = next_pad;
            field_index++;
        }

        if (format->char_suffix) {
            if (blank_fields > 0u) {
                if (format->intrinsic_width > 0u && blank_fields > SIZE_MAX / format->intrinsic_width) {
                    return false;
                }
                bx_od_print_spaces(blank_fields * format->intrinsic_width);
            }
            bx_od_print_spaces(pad_remaining);
            bx_od_print_ascii_trailer(buffer, size);
        }

        fputc('\n', stdout);
    }

    return true;
}

static void bx_od_print_final_line(const struct bx_od_options* options,
                                   uintmax_t address,
                                   bool have_label,
                                   uintmax_t label) {
    char prefix[256];

    bx_od_build_prefix(prefix, sizeof(prefix), options->address_radix, address, have_label, label);
    if (prefix[0] != '\0') {
        puts(prefix);
    }
}

static bool bx_od_dump_regular(const struct bx_od_options* options,
                               const struct bx_od_operands* operands,
                               struct bx_diag_ctx* diag) {
    struct bx_od_input input;
    unsigned char* buffer = xmalloc((size_t)options->width);
    unsigned char* previous = options->output_duplicates ? NULL : xmalloc((size_t)options->width);
    size_t previous_size = 0u;
    bool previous_valid = false;
    bool suppressed = false;
    bool width_warning_emitted = false;
    uintmax_t dumped = 0u;
    uintmax_t address = operands->offset;
    uintmax_t label = operands->label;

    bx_od_input_init(&input, operands, diag);

    if (!bx_od_input_skip(&input, operands->offset, bx_od_operands_are_stdin_only(operands))) {
        if (!bx_od_input_had_error(&input)) {
            bx_diag(diag, "cannot skip past end of combined input");
        }
        bx_od_input_close_current(&input);
        free(buffer);
        free(previous);
        return false;
    }

    bx_od_input_prime_zero_read(&input, options);
    bx_od_maybe_warn_width_adjustment(options, &input, &width_warning_emitted);

    while (true) {
        size_t to_read = (size_t)options->width;
        size_t nread;

        if (options->read_bytes_set) {
            uintmax_t remaining = options->read_bytes - dumped;
            if (dumped >= options->read_bytes) {
                break;
            }
            if (remaining < (uintmax_t)to_read) {
                to_read = (size_t)remaining;
            }
        }

        nread = bx_od_input_read(&input, buffer, to_read);
        bx_od_maybe_warn_width_adjustment(options, &input, &width_warning_emitted);
        if (nread == 0u) {
            break;
        }

        if (!options->output_duplicates &&
            previous_valid &&
            nread == previous_size &&
            memcmp(previous, buffer, nread) == 0) {
            if (!suppressed) {
                puts("*");
                suppressed = true;
            }
        }
        else {
            suppressed = false;
            if (!bx_od_print_block(options, buffer, nread, address, operands->have_label, label)) {
                bx_diag(diag, "failed to render output");
                bx_od_input_close_current(&input);
                free(buffer);
                free(previous);
                return false;
            }
        }

        if (!options->output_duplicates) {
            memcpy(previous, buffer, nread);
            previous_size = nread;
            previous_valid = true;
        }

        dumped += (uintmax_t)nread;
        address = operands->offset + dumped;
        if (operands->have_label) {
            label = operands->label + dumped;
        }
    }

    bx_od_input_close_current(&input);
    if (dumped > 0u || bx_od_input_opened_any(&input)) {
        bx_od_print_final_line(options, address, operands->have_label, label);
    }
    free(buffer);
    free(previous);
    return true;
}

static void bx_od_print_string_line(const struct bx_od_options* options,
                                    uintmax_t address,
                                    bool have_label,
                                    uintmax_t label,
                                    const char* text) {
    char prefix[256];

    bx_od_build_prefix(prefix, sizeof(prefix), options->address_radix, address, have_label, label);
    if (prefix[0] != '\0') {
        printf("%s ", prefix);
    }
    puts(text);
}

static bool bx_od_dump_strings(const struct bx_od_options* options,
                               const struct bx_od_operands* operands,
                               struct bx_diag_ctx* diag) {
    struct bx_od_input input;
    unsigned char chunk[4096];
    char* string_buf = NULL;
    size_t string_cap = 0u;
    size_t string_len = 0u;
    bool in_string = false;
    bool width_warning_emitted = false;
    uintmax_t string_start = operands->offset;
    uintmax_t position = operands->offset;
    uintmax_t remaining = options->read_bytes_set ? options->read_bytes : UINTMAX_MAX;

    bx_od_input_init(&input, operands, diag);

    if (!bx_od_input_skip(&input, operands->offset, bx_od_operands_are_stdin_only(operands))) {
        if (!bx_od_input_had_error(&input)) {
            bx_diag(diag, "cannot skip past end of combined input");
        }
        bx_od_input_close_current(&input);
        free(string_buf);
        return false;
    }

    bx_od_input_prime_zero_read(&input, options);
    bx_od_maybe_warn_width_adjustment(options, &input, &width_warning_emitted);

    while (remaining > 0u) {
        size_t to_read = sizeof(chunk);
        size_t nread;

        if (options->read_bytes_set && remaining < (uintmax_t)to_read) {
            to_read = (size_t)remaining;
        }

        nread = bx_od_input_read(&input, chunk, to_read);
        bx_od_maybe_warn_width_adjustment(options, &input, &width_warning_emitted);
        if (nread == 0u) {
            break;
        }

        for (size_t i = 0u; i < nread; i++) {
            unsigned char ch = chunk[i];

            if (isprint(ch)) {
                if (!in_string) {
                    in_string = true;
                    string_start = position;
                    string_len = 0u;
                }

                if (string_len + 1u >= string_cap) {
                    size_t new_cap = (string_cap == 0u) ? 64u : string_cap * 2u;
                    string_buf = xrealloc(string_buf, new_cap);
                    string_cap = new_cap;
                }

                string_buf[string_len++] = (char)ch;
            }
            else if (ch == '\0') {
                uintmax_t label = 0u;

                if (!in_string) {
                    string_start = position;
                    string_len = 0u;
                }

                if (string_buf == NULL) {
                    string_buf = xmalloc(1u);
                    string_cap = 1u;
                }

                string_buf[string_len] = '\0';
                if ((uintmax_t)string_len >= options->strings_min) {
                    if (operands->have_label) {
                        label = operands->label + (string_start - operands->offset);
                    }
                    bx_od_print_string_line(options, string_start, operands->have_label, label, string_buf);
                }

                in_string = false;
                string_len = 0u;
            }
            else {
                in_string = false;
                string_len = 0u;
            }

            position++;
        }

        if (options->read_bytes_set) {
            remaining -= (uintmax_t)nread;
        }
    }

    if (options->read_bytes_set && remaining == 0u && in_string && (uintmax_t)string_len >= options->strings_min) {
        uintmax_t label = 0u;

        string_buf[string_len] = '\0';
        if (operands->have_label) {
            label = operands->label + (string_start - operands->offset);
        }
        bx_od_print_string_line(options, string_start, operands->have_label, label, string_buf);
    }

    bx_od_input_close_current(&input);
    free(string_buf);
    return true;
}

int bx_od_main(int argc, char** argv) {
    struct bx_od_options options;
    struct bx_od_operands operands;
    struct bx_diag_ctx diag = {.progname = "od", .exit_status = 0};
    int first_operand = 0;
    bool ok;

    if (!bx_od_parse_options(argc, argv, &options, &first_operand, &diag)) {
        free(options.formats);
        return 1;
    }

    if (options.show_help) {
        bx_od_print_help(stdout, options.progname);
        free(options.formats);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        free(options.formats);
        return 0;
    }

    if (!bx_od_parse_operands(argc, argv, first_operand, &options, &operands, &diag)) {
        free(options.formats);
        return 1;
    }

    if (options.strings_mode) {
        ok = bx_od_dump_strings(&options, &operands, &diag);
    }
    else {
        ok = bx_od_dump_regular(&options, &operands, &diag);
    }

    free((void*)operands.files);
    free(options.formats);
    return ok ? diag.exit_status : 1;
}
