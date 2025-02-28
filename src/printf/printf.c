#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_printf_options {
    const char* progname;
    const char* format;
    int first_argument;
    bool show_help;
    bool show_version;
};

enum bx_printf_length {
    BX_PRINTF_LEN_NONE = 0,
    BX_PRINTF_LEN_HH,
    BX_PRINTF_LEN_H,
    BX_PRINTF_LEN_L,
    BX_PRINTF_LEN_LL,
    BX_PRINTF_LEN_J,
    BX_PRINTF_LEN_Z,
    BX_PRINTF_LEN_T,
    BX_PRINTF_LEN_CAP_L,
    BX_PRINTF_LEN_Q,
    BX_PRINTF_LEN_CAP_Z,
};

struct bx_printf_spec {
    char conversion;
    enum bx_printf_length length;

    char flags[16];
    size_t flags_len;

    bool has_width;
    bool width_from_arg;
    int width_value;
    int width_position;

    bool has_precision;
    bool precision_from_arg;
    int precision_value;
    int precision_position;

    bool value_consumes_argument;
    int value_position;

    char canonical[128];
};

struct bx_printf_byte_buffer {
    unsigned char* data;
    size_t len;
    size_t cap;
};

enum bx_printf_position_parse_result {
    BX_PRINTF_POSITION_NONE = 0,
    BX_PRINTF_POSITION_OK,
    BX_PRINTF_POSITION_INVALID,
};

static const char* bx_printf_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "printf";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_printf_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s FORMAT [ARGUMENT]...\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Print ARGUMENT(s) according to FORMAT, or execute according to OPTION:\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "FORMAT controls the output as in C printf.  Interpreted sequences are:\n");
    fprintf(stream, "\n");
    fprintf(stream, "  \\\"      double quote\n");
    fprintf(stream, "  \\\\      backslash\n");
    fprintf(stream, "  \\a      alert (BEL)\n");
    fprintf(stream, "  \\b      backspace\n");
    fprintf(stream, "  \\c      produce no further output\n");
    fprintf(stream, "  \\e      escape\n");
    fprintf(stream, "  \\f      form feed\n");
    fprintf(stream, "  \\n      new line\n");
    fprintf(stream, "  \\r      carriage return\n");
    fprintf(stream, "  \\t      horizontal tab\n");
    fprintf(stream, "  \\v      vertical tab\n");
    fprintf(stream, "  \\NNN    byte with octal value NNN (1 to 3 digits)\n");
    fprintf(stream, "  \\xHH    byte with hexadecimal value HH (1 to 2 digits)\n");
    fprintf(stream, "  \\uHHHH  Unicode (ISO/IEC 10646) character with hex value HHHH (4 digits)\n");
    fprintf(stream, "  \\UHHHHHHHH  Unicode character with hex value HHHHHHHH (8 digits)\n");
    fprintf(stream, "  %%%%      a single %%\n");
    fprintf(stream, "  %%b      ARGUMENT as a string with '\\' escapes interpreted,\n");
    fprintf(stream, "          except that octal escapes should have a leading 0 like \\0NNN\n");
    fprintf(stream, "  %%q      ARGUMENT is printed in a format that can be reused as shell input,\n");
    fprintf(stream, "          escaping non-printable characters with the POSIX $'' syntax\n");
    fprintf(stream, "\n");
    fprintf(stream, "and all C format specifications ending with one of diouxXfeEgGcs, with\n");
    fprintf(stream, "ARGUMENTs converted to proper type first.  Variable widths are handled.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Your shell may have its own version of printf, which usually supersedes\n");
    fprintf(stream, "the version described here.  Please refer to your shell's documentation\n");
    fprintf(stream, "for details about the options it supports.\n");
}

static void bx_printf_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_printf_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_printf_parse_options(int argc, char** argv, struct bx_printf_options* options, struct bx_diag_ctx* diag) {
    memset(options, 0, sizeof(*options));
    options->progname = bx_printf_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    if (argc < 2) {
        bx_diag(diag, "missing operand");
        bx_printf_print_try_help(options->progname);
        return false;
    }

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        options->show_help = true;
        return true;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        options->show_version = true;
        return true;
    }

    int format_index = 1;
    if (strcmp(argv[1], "--") == 0) {
        format_index = 2;
    }

    if (format_index >= argc) {
        bx_diag(diag, "missing operand");
        bx_printf_print_try_help(options->progname);
        return false;
    }

    options->format = argv[format_index];
    options->first_argument = format_index + 1;
    return true;
}

static bool bx_printf_is_flag_char(char ch) {
    return (ch == '#') || (ch == '0') || (ch == '-') || (ch == ' ') || (ch == '+') || (ch == '\'') || (ch == 'I');
}

static bool bx_printf_spec_has_flag(const struct bx_printf_spec* spec, char flag) {
    for (size_t i = 0; i < spec->flags_len; i++) {
        if (spec->flags[i] == flag) {
            return true;
        }
    }
    return false;
}

static int bx_printf_hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') {
        return (int)(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (int)(ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (int)(ch - 'A');
    }
    return -1;
}

static enum bx_printf_position_parse_result bx_printf_try_parse_position(const char* text, const char** end_out, int* position_out) {
    if (text == NULL || !isdigit((unsigned char)text[0])) {
        return BX_PRINTF_POSITION_NONE;
    }

    const char* p = text;
    unsigned long long value = 0;
    bool overflow = false;

    while (isdigit((unsigned char)*p)) {
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (ULLONG_MAX - digit) / 10u) {
            overflow = true;
        }
        else {
            value = value * 10u + digit;
        }
        p++;
    }

    if (*p != '$') {
        return BX_PRINTF_POSITION_NONE;
    }

    if (overflow || value == 0u || value > (unsigned long long)INT_MAX) {
        return BX_PRINTF_POSITION_INVALID;
    }

    *end_out = p + 1;
    *position_out = (int)value;
    return BX_PRINTF_POSITION_OK;
}

static int bx_printf_parse_decimal_constant(const char** cursor) {
    const char* p = *cursor;
    int value = 0;

    while (isdigit((unsigned char)*p)) {
        int digit = (int)(*p - '0');
        if (value > (INT_MAX - digit) / 10) {
            value = INT_MAX;
        }
        else {
            value = value * 10 + digit;
        }
        p++;
    }

    *cursor = p;
    return value;
}

static bool bx_printf_is_integer_conversion(char conv) {
    return conv == 'd' || conv == 'i' || conv == 'o' || conv == 'u' || conv == 'x' || conv == 'X';
}

static bool bx_printf_is_float_conversion(char conv) {
    return conv == 'a' || conv == 'A' || conv == 'e' || conv == 'E' || conv == 'f' || conv == 'F' || conv == 'g' || conv == 'G';
}

static bool bx_printf_append_text(char* buffer, size_t buffer_size, size_t* len, const char* text) {
    size_t text_len = strlen(text);
    if (*len + text_len >= buffer_size) {
        return false;
    }

    memcpy(buffer + *len, text, text_len);
    *len += text_len;
    buffer[*len] = '\0';
    return true;
}

static bool bx_printf_append_char(char* buffer, size_t buffer_size, size_t* len, char ch) {
    if (*len + 1u >= buffer_size) {
        return false;
    }

    buffer[*len] = ch;
    (*len)++;
    buffer[*len] = '\0';
    return true;
}

static bool bx_printf_append_int(char* buffer, size_t buffer_size, size_t* len, int value) {
    char temp[32];
    int written = snprintf(temp, sizeof(temp), "%d", value);
    if (written < 0 || (size_t)written >= sizeof(temp)) {
        return false;
    }
    return bx_printf_append_text(buffer, buffer_size, len, temp);
}

static bool bx_printf_build_canonical_format(struct bx_printf_spec* spec) {
    size_t len = 0;

    spec->canonical[0] = '\0';
    if (!bx_printf_append_text(spec->canonical, sizeof(spec->canonical), &len, "%")) {
        return false;
    }

    if (spec->flags_len > 0) {
        if (len + spec->flags_len >= sizeof(spec->canonical)) {
            return false;
        }
        memcpy(spec->canonical + len, spec->flags, spec->flags_len);
        len += spec->flags_len;
        spec->canonical[len] = '\0';
    }

    if (spec->has_width) {
        if (spec->width_from_arg) {
            if (!bx_printf_append_text(spec->canonical, sizeof(spec->canonical), &len, "*")) {
                return false;
            }
        }
        else if (!bx_printf_append_int(spec->canonical, sizeof(spec->canonical), &len, spec->width_value)) {
            return false;
        }
    }

    if (spec->has_precision) {
        if (!bx_printf_append_text(spec->canonical, sizeof(spec->canonical), &len, ".")) {
            return false;
        }

        if (spec->precision_from_arg) {
            if (!bx_printf_append_text(spec->canonical, sizeof(spec->canonical), &len, "*")) {
                return false;
            }
        }
        else if (!bx_printf_append_int(spec->canonical, sizeof(spec->canonical), &len, spec->precision_value)) {
            return false;
        }
    }

    const char* length_text = "";
    switch (spec->length) {
        case BX_PRINTF_LEN_NONE:
            length_text = "";
            break;
        case BX_PRINTF_LEN_HH:
            length_text = "hh";
            break;
        case BX_PRINTF_LEN_H:
            length_text = "h";
            break;
        case BX_PRINTF_LEN_L:
            if (spec->conversion == 'c' || spec->conversion == 's') {
                length_text = "";
            }
            else {
                length_text = "l";
            }
            break;
        case BX_PRINTF_LEN_LL:
            length_text = "ll";
            break;
        case BX_PRINTF_LEN_J:
            length_text = "j";
            break;
        case BX_PRINTF_LEN_Z:
        case BX_PRINTF_LEN_CAP_Z:
            length_text = "z";
            break;
        case BX_PRINTF_LEN_T:
            length_text = "t";
            break;
        case BX_PRINTF_LEN_CAP_L:
            length_text = "L";
            break;
        case BX_PRINTF_LEN_Q:
            length_text = "ll";
            break;
    }

    if (!bx_printf_append_text(spec->canonical, sizeof(spec->canonical), &len, length_text)) {
        return false;
    }

    if (len + 1 >= sizeof(spec->canonical)) {
        return false;
    }
    spec->canonical[len++] = spec->conversion;
    spec->canonical[len] = '\0';

    return true;
}

static bool bx_printf_set_max_position(int candidate, int* max_position) {
    if (candidate > *max_position) {
        *max_position = candidate;
    }
    return true;
}

static bool bx_printf_parse_spec(const char** cursor, struct bx_printf_spec* spec, int* next_auto_position, int* max_position, struct bx_diag_ctx* diag) {
    const char* p = *cursor;
    memset(spec, 0, sizeof(*spec));

    bool explicit_value_position = false;
    int parsed_value_position = 0;
    const char* after_position = NULL;

    enum bx_printf_position_parse_result position_result = bx_printf_try_parse_position(p, &after_position, &parsed_value_position);
    if (position_result == BX_PRINTF_POSITION_INVALID) {
        bx_diag(diag, "invalid conversion specification");
        return false;
    }
    if (position_result == BX_PRINTF_POSITION_OK) {
        explicit_value_position = true;
        p = after_position;
    }

    while (bx_printf_is_flag_char(*p)) {
        if (spec->flags_len + 1u < sizeof(spec->flags)) {
            spec->flags[spec->flags_len++] = *p;
            spec->flags[spec->flags_len] = '\0';
        }
        p++;
    }

    if (*p == '*') {
        spec->has_width = true;
        spec->width_from_arg = true;
        p++;

        position_result = bx_printf_try_parse_position(p, &after_position, &spec->width_position);
        if (position_result == BX_PRINTF_POSITION_INVALID) {
            bx_diag(diag, "invalid conversion specification");
            return false;
        }
        if (position_result == BX_PRINTF_POSITION_OK) {
            p = after_position;
        }
        else {
            if (*next_auto_position <= 0 || *next_auto_position == INT_MAX) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            spec->width_position = (*next_auto_position)++;
        }

        bx_printf_set_max_position(spec->width_position, max_position);
    }
    else if (isdigit((unsigned char)*p)) {
        spec->has_width = true;
        spec->width_value = bx_printf_parse_decimal_constant(&p);
    }

    if (*p == '.') {
        spec->has_precision = true;
        p++;

        if (*p == '*') {
            spec->precision_from_arg = true;
            p++;

            position_result = bx_printf_try_parse_position(p, &after_position, &spec->precision_position);
            if (position_result == BX_PRINTF_POSITION_INVALID) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            if (position_result == BX_PRINTF_POSITION_OK) {
                p = after_position;
            }
            else {
                if (*next_auto_position <= 0 || *next_auto_position == INT_MAX) {
                    bx_diag(diag, "invalid conversion specification");
                    return false;
                }
                spec->precision_position = (*next_auto_position)++;
            }

            bx_printf_set_max_position(spec->precision_position, max_position);
        }
        else if (isdigit((unsigned char)*p)) {
            spec->precision_value = bx_printf_parse_decimal_constant(&p);
        }
        else {
            spec->precision_value = 0;
        }
    }

    if (p[0] == 'h' && p[1] == 'h') {
        spec->length = BX_PRINTF_LEN_HH;
        p += 2;
    }
    else if (p[0] == 'l' && p[1] == 'l') {
        spec->length = BX_PRINTF_LEN_LL;
        p += 2;
    }
    else if (p[0] == 'h') {
        spec->length = BX_PRINTF_LEN_H;
        p++;
    }
    else if (p[0] == 'l') {
        spec->length = BX_PRINTF_LEN_L;
        p++;
    }
    else if (p[0] == 'j') {
        spec->length = BX_PRINTF_LEN_J;
        p++;
    }
    else if (p[0] == 'z') {
        spec->length = BX_PRINTF_LEN_Z;
        p++;
    }
    else if (p[0] == 't') {
        spec->length = BX_PRINTF_LEN_T;
        p++;
    }
    else if (p[0] == 'L') {
        spec->length = BX_PRINTF_LEN_CAP_L;
        p++;
    }
    if (*p == '\0') {
        bx_diag(diag, "invalid conversion specification");
        return false;
    }

    char conv = *p++;
    if (conv == 'C') {
        conv = 'c';
        if (spec->length == BX_PRINTF_LEN_NONE) {
            spec->length = BX_PRINTF_LEN_L;
        }
    }
    else if (conv == 'S') {
        conv = 's';
        if (spec->length == BX_PRINTF_LEN_NONE) {
            spec->length = BX_PRINTF_LEN_L;
        }
    }

    spec->conversion = conv;

    switch (conv) {
        case '%':
            spec->value_consumes_argument = false;
            if (explicit_value_position || spec->flags_len != 0 || spec->has_width || spec->has_precision || spec->length != BX_PRINTF_LEN_NONE) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        case 'm':
            spec->value_consumes_argument = false;
            if (explicit_value_position) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        case 'b':
            spec->value_consumes_argument = true;
            if (spec->length != BX_PRINTF_LEN_NONE) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        case 'q':
            spec->value_consumes_argument = true;
            if (spec->flags_len != 0 || spec->has_width || spec->has_precision || spec->length != BX_PRINTF_LEN_NONE) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        case 'n':
            bx_diag(diag, "invalid conversion specification");
            return false;

        case 'p':
            spec->value_consumes_argument = true;
            if (spec->length != BX_PRINTF_LEN_NONE) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        case 'c':
        case 's':
            spec->value_consumes_argument = true;
            if (spec->length != BX_PRINTF_LEN_NONE && spec->length != BX_PRINTF_LEN_L) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;

        default:
            if (bx_printf_is_integer_conversion(conv)) {
                spec->value_consumes_argument = true;
                if (spec->length == BX_PRINTF_LEN_CAP_L) {
                    spec->length = BX_PRINTF_LEN_LL;
                }
            }
            else if (bx_printf_is_float_conversion(conv)) {
                spec->value_consumes_argument = true;
                if (spec->length != BX_PRINTF_LEN_NONE && spec->length != BX_PRINTF_LEN_L && spec->length != BX_PRINTF_LEN_CAP_L) {
                    bx_diag(diag, "invalid conversion specification");
                    return false;
                }
            }
            else {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            break;
    }

    if (spec->value_consumes_argument) {
        if (explicit_value_position) {
            spec->value_position = parsed_value_position;
        }
        else {
            if (*next_auto_position <= 0 || *next_auto_position == INT_MAX) {
                bx_diag(diag, "invalid conversion specification");
                return false;
            }
            spec->value_position = (*next_auto_position)++;
        }

        bx_printf_set_max_position(spec->value_position, max_position);
    }
    else if (explicit_value_position) {
        bx_diag(diag, "invalid conversion specification");
        return false;
    }

    if (!bx_printf_build_canonical_format(spec)) {
        bx_diag(diag, "invalid conversion specification");
        return false;
    }

    *cursor = p;
    return true;
}

static const char* bx_printf_get_argument(const char* const* arguments, size_t argument_count, size_t base, int position) {
    if (position <= 0) {
        return NULL;
    }

    size_t offset = (size_t)(position - 1);
    if (base > SIZE_MAX - offset) {
        return NULL;
    }

    size_t index = base + offset;
    if (index >= argument_count) {
        return NULL;
    }

    return arguments[index];
}

static void bx_printf_numeric_error(struct bx_diag_ctx* diag, const char* text, const char* message) {
    if (text == NULL) {
        return;
    }

    bx_diag(diag, "'%s': %s", text, message);
}

static bool bx_printf_parse_character_constant(const char* text, uintmax_t* value_out) {
    if (text == NULL) {
        return false;
    }

    if ((text[0] == '\'' || text[0] == '"') && text[1] != '\0') {
        *value_out = (uintmax_t)(unsigned char)text[1];
        return true;
    }

    return false;
}

static intmax_t bx_printf_parse_signed_value(const char* text, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        return 0;
    }

    uintmax_t char_constant = 0;
    if (bx_printf_parse_character_constant(text, &char_constant)) {
        return (intmax_t)char_constant;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 0);

    if (end == text) {
        bx_printf_numeric_error(diag, text, "expected a numeric value");
        return 0;
    }

    if (errno == ERANGE) {
        bx_printf_numeric_error(diag, text, "numerical result out of range");
    }

    if (end != NULL && *end != '\0') {
        bx_printf_numeric_error(diag, text, "value not completely converted");
    }

    return value;
}

static uintmax_t bx_printf_parse_unsigned_value(const char* text, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        return 0;
    }

    uintmax_t char_constant = 0;
    if (bx_printf_parse_character_constant(text, &char_constant)) {
        return char_constant;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 0);

    if (end == text) {
        bx_printf_numeric_error(diag, text, "expected a numeric value");
        return 0;
    }

    if (errno == ERANGE) {
        bx_printf_numeric_error(diag, text, "numerical result out of range");
    }

    if (end != NULL && *end != '\0') {
        bx_printf_numeric_error(diag, text, "value not completely converted");
    }

    return value;
}

static long double bx_printf_parse_float_value(const char* text, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        return 0.0L;
    }

    uintmax_t char_constant = 0;
    if (bx_printf_parse_character_constant(text, &char_constant)) {
        return (long double)char_constant;
    }

    errno = 0;
    char* end = NULL;
    long double value = strtold(text, &end);

    if (end == text) {
        bx_printf_numeric_error(diag, text, "expected a numeric value");
        return 0.0L;
    }

    if (errno == ERANGE) {
        bx_printf_numeric_error(diag, text, "numerical result out of range");
    }

    if (end != NULL && *end != '\0') {
        bx_printf_numeric_error(diag, text, "value not completely converted");
    }

    return value;
}

static int bx_printf_parse_int_value(const char* text, struct bx_diag_ctx* diag) {
    intmax_t parsed = bx_printf_parse_signed_value(text, diag);

    if (parsed > (intmax_t)INT_MAX) {
        if (text != NULL) {
            bx_printf_numeric_error(diag, text, "numerical result out of range");
        }
        return INT_MAX;
    }
    if (parsed < (intmax_t)INT_MIN) {
        if (text != NULL) {
            bx_printf_numeric_error(diag, text, "numerical result out of range");
        }
        return INT_MIN;
    }

    return (int)parsed;
}

static size_t bx_printf_utf8_encode(uint32_t codepoint, unsigned char out[4]) {
    if (codepoint <= 0x7Fu) {
        out[0] = (unsigned char)codepoint;
        return 1;
    }

    if (codepoint <= 0x7FFu) {
        out[0] = (unsigned char)(0xC0u | (codepoint >> 6));
        out[1] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }

    if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
        return 0;
    }

    if (codepoint <= 0xFFFFu) {
        out[0] = (unsigned char)(0xE0u | (codepoint >> 12));
        out[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }

    if (codepoint <= 0x10FFFFu) {
        out[0] = (unsigned char)(0xF0u | (codepoint >> 18));
        out[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
        out[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[3] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
        return 4;
    }

    return 0;
}

static bool bx_printf_decode_escape(const char** cursor, bool allow_stop, unsigned char out[8], size_t* out_len, bool* stop_output, struct bx_diag_ctx* diag) {
    const char* p = *cursor;
    *out_len = 0;
    *stop_output = false;

    if (*p == '\0') {
        out[0] = '\\';
        *out_len = 1;
        return true;
    }

    unsigned char esc = (unsigned char)*p++;
    switch (esc) {
        case 'a':
            out[0] = '\a';
            *out_len = 1;
            break;
        case 'b':
            out[0] = '\b';
            *out_len = 1;
            break;
        case 'e':
            out[0] = 0x1B;
            *out_len = 1;
            break;
        case 'f':
            out[0] = '\f';
            *out_len = 1;
            break;
        case 'n':
            out[0] = '\n';
            *out_len = 1;
            break;
        case 'r':
            out[0] = '\r';
            *out_len = 1;
            break;
        case 't':
            out[0] = '\t';
            *out_len = 1;
            break;
        case 'v':
            out[0] = '\v';
            *out_len = 1;
            break;
        case '\\':
            out[0] = '\\';
            *out_len = 1;
            break;
        case '\"':
            out[0] = '\"';
            *out_len = 1;
            break;
        case '\'':
            out[0] = '\'';
            *out_len = 1;
            break;

        case 'c':
            if (allow_stop) {
                *stop_output = true;
                *cursor = p;
                return true;
            }
            out[0] = '\\';
            out[1] = 'c';
            *out_len = 2;
            break;

        case 'x': {
            int digit = bx_printf_hex_value((unsigned char)*p);
            if (digit < 0) {
                bx_diag(diag, "missing hexadecimal number in escape");
                return false;
            }

            unsigned int value = 0;
            int digits = 0;
            while (digits < 2) {
                digit = bx_printf_hex_value((unsigned char)*p);
                if (digit < 0) {
                    break;
                }
                value = value * 16u + (unsigned int)digit;
                p++;
                digits++;
            }

            out[0] = (unsigned char)(value & 0xFFu);
            *out_len = 1;
            break;
        }

        case 'u':
        case 'U': {
            int expected_digits = (esc == 'u') ? 4 : 8;
            uint32_t value = 0;
            for (int i = 0; i < expected_digits; i++) {
                int digit = bx_printf_hex_value((unsigned char)p[i]);
                if (digit < 0) {
                    bx_diag(diag, "missing hexadecimal number in escape");
                    return false;
                }
                value = (value * 16u) + (uint32_t)digit;
            }
            p += expected_digits;

            size_t encoded = bx_printf_utf8_encode(value, out);
            if (encoded == 0) {
                out[0] = '?';
                *out_len = 1;
            }
            else {
                *out_len = encoded;
            }
            break;
        }

        default:
            if (esc >= '0' && esc <= '7') {
                unsigned int value = (unsigned int)(esc - '0');
                int digits = 1;
                while (digits < 3 && *p >= '0' && *p <= '7') {
                    value = value * 8u + (unsigned int)(*p - '0');
                    p++;
                    digits++;
                }
                out[0] = (unsigned char)(value & 0xFFu);
                *out_len = 1;
            }
            else {
                out[0] = '\\';
                out[1] = esc;
                *out_len = 2;
            }
            break;
    }

    *cursor = p;
    return true;
}

static void bx_printf_byte_buffer_init(struct bx_printf_byte_buffer* buffer) {
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static void bx_printf_byte_buffer_free(struct bx_printf_byte_buffer* buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->cap = 0;
}

static bool bx_printf_byte_buffer_reserve(struct bx_printf_byte_buffer* buffer, size_t additional) {
    if (additional == 0) {
        return true;
    }

    if (buffer->len > SIZE_MAX - additional) {
        return false;
    }

    size_t needed = buffer->len + additional;
    if (needed <= buffer->cap) {
        return true;
    }

    size_t new_cap = (buffer->cap == 0) ? 32u : buffer->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }

    buffer->data = xrealloc(buffer->data, new_cap);
    buffer->cap = new_cap;
    return true;
}

static bool bx_printf_byte_buffer_append(struct bx_printf_byte_buffer* buffer, const unsigned char* data, size_t len) {
    if (!bx_printf_byte_buffer_reserve(buffer, len)) {
        return false;
    }

    if (len > 0) {
        memcpy(buffer->data + buffer->len, data, len);
        buffer->len += len;
    }
    return true;
}

static bool bx_printf_write_bytes(struct bx_diag_ctx* diag, const unsigned char* data, size_t len) {
    if (len == 0) {
        return true;
    }

    if (fwrite(data, 1, len, stdout) != len) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_printf_write_padding(struct bx_diag_ctx* diag, size_t count) {
    static const unsigned char spaces[64] = {
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
        ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    };

    while (count > 0) {
        size_t chunk = (count > sizeof(spaces)) ? sizeof(spaces) : count;
        if (!bx_printf_write_bytes(diag, spaces, chunk)) {
            return false;
        }
        count -= chunk;
    }

    return true;
}

static bool bx_printf_is_shell_safe_char(unsigned char ch) {
    if (isalnum(ch)) {
        return true;
    }

    switch (ch) {
        case '%':
        case '+':
        case ',':
        case '-':
        case '.':
        case '/':
        case ':':
        case '@':
        case ']':
        case '_':
            return true;
        default:
            return false;
    }
}

static bool bx_printf_is_shell_safe_string(const char* text) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    size_t text_len = strlen(text);
    const unsigned char* p = (const unsigned char*)text;
    size_t index = 0;
    while (*p != '\0') {
        unsigned char ch = *p;
        if ((ch == '~' || ch == '#') && index > 0u) {
            p++;
            index++;
            continue;
        }
        if ((ch == '{' || ch == '}') && text_len > 1u) {
            p++;
            index++;
            continue;
        }
        if (!bx_printf_is_shell_safe_char(ch)) {
            return false;
        }
        p++;
        index++;
    }

    return true;
}

static bool bx_printf_is_shell_double_quote_char(unsigned char ch) {
    return ch == ' ' || ch == '\'' || bx_printf_is_shell_safe_char(ch);
}

static bool bx_printf_can_use_double_quotes(const char* text) {
    bool has_single_quote = false;
    const unsigned char* p = (const unsigned char*)text;
    size_t index = 0;

    while (*p != '\0') {
        unsigned char ch = *p++;
        if (!isprint(ch)) {
            return false;
        }
        if (ch == '\'') {
            has_single_quote = true;
        }
        if ((ch == '#' || ch == '~') && index == 0u) {
            index++;
            continue;
        }
        if (!bx_printf_is_shell_double_quote_char(ch)) {
            return false;
        }
        index++;
    }

    return has_single_quote;
}

static bool bx_printf_append_dollar_escape(char* buffer, size_t buffer_size, size_t* len, unsigned char ch) {
    switch (ch) {
        case '\a':
            return bx_printf_append_text(buffer, buffer_size, len, "\\a");
        case '\b':
            return bx_printf_append_text(buffer, buffer_size, len, "\\b");
        case '\t':
            return bx_printf_append_text(buffer, buffer_size, len, "\\t");
        case '\n':
            return bx_printf_append_text(buffer, buffer_size, len, "\\n");
        case '\v':
            return bx_printf_append_text(buffer, buffer_size, len, "\\v");
        case '\f':
            return bx_printf_append_text(buffer, buffer_size, len, "\\f");
        case '\r':
            return bx_printf_append_text(buffer, buffer_size, len, "\\r");
        default: {
            char octal_escape[5];
            int written = snprintf(octal_escape, sizeof(octal_escape), "\\%03o", (unsigned int)ch);
            if (written < 0 || (size_t)written >= sizeof(octal_escape)) {
                return false;
            }
            return bx_printf_append_text(buffer, buffer_size, len, octal_escape);
        }
    }
}

static char* bx_printf_quote_shell(const char* text) {
    enum bx_printf_quote_mode {
        BX_PRINTF_QUOTE_MODE_NONE = 0,
        BX_PRINTF_QUOTE_MODE_SINGLE,
        BX_PRINTF_QUOTE_MODE_DOLLAR,
    };

    if (text == NULL) {
        text = "";
    }

    if (text[0] == '\0') {
        return xstrdup("''");
    }

    if (bx_printf_is_shell_safe_string(text)) {
        return xstrdup(text);
    }

    size_t text_len = strlen(text);
    if (bx_printf_can_use_double_quotes(text)) {
        if (text_len > (SIZE_MAX - 3u)) {
            return NULL;
        }

        char* out = xmalloc(text_len + 3u);
        size_t out_len = 0;
        out[0] = '\0';

        if (!bx_printf_append_char(out, text_len + 3u, &out_len, '\"')) {
            free(out);
            return NULL;
        }
        if (!bx_printf_append_text(out, text_len + 3u, &out_len, text)) {
            free(out);
            return NULL;
        }
        if (!bx_printf_append_char(out, text_len + 3u, &out_len, '\"')) {
            free(out);
            return NULL;
        }
        return out;
    }

    if (text_len > (SIZE_MAX - 8u) / 12u) {
        return NULL;
    }

    size_t out_cap = text_len * 12u + 8u;
    char* out = xmalloc(out_cap);
    size_t out_len = 0;
    enum bx_printf_quote_mode mode = BX_PRINTF_QUOTE_MODE_NONE;
    bool emitted = false;

    out[0] = '\0';
    for (size_t i = 0; i < text_len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (!isprint(ch)) {
            if (mode == BX_PRINTF_QUOTE_MODE_SINGLE) {
                if (!bx_printf_append_char(out, out_cap, &out_len, '\'')) {
                    free(out);
                    return NULL;
                }
                mode = BX_PRINTF_QUOTE_MODE_NONE;
            }
            if (mode != BX_PRINTF_QUOTE_MODE_DOLLAR) {
                if (!emitted) {
                    if (!bx_printf_append_text(out, out_cap, &out_len, "''")) {
                        free(out);
                        return NULL;
                    }
                    emitted = true;
                }
                if (!bx_printf_append_text(out, out_cap, &out_len, "$'")) {
                    free(out);
                    return NULL;
                }
                mode = BX_PRINTF_QUOTE_MODE_DOLLAR;
            }
            if (!bx_printf_append_dollar_escape(out, out_cap, &out_len, ch)) {
                free(out);
                return NULL;
            }
            emitted = true;
            continue;
        }

        if (mode == BX_PRINTF_QUOTE_MODE_DOLLAR) {
            if (!bx_printf_append_char(out, out_cap, &out_len, '\'')) {
                free(out);
                return NULL;
            }
            mode = BX_PRINTF_QUOTE_MODE_NONE;
        }

        if (ch == '\'') {
            if (mode == BX_PRINTF_QUOTE_MODE_SINGLE) {
                if (!bx_printf_append_char(out, out_cap, &out_len, '\'')) {
                    free(out);
                    return NULL;
                }
                mode = BX_PRINTF_QUOTE_MODE_NONE;
            }
            if (!emitted) {
                if (!bx_printf_append_text(out, out_cap, &out_len, "''")) {
                    free(out);
                    return NULL;
                }
                emitted = true;
            }
            if (!bx_printf_append_text(out, out_cap, &out_len, "\\'")) {
                free(out);
                return NULL;
            }
            bool next_is_printable_non_quote = false;
            if (i + 1u < text_len) {
                unsigned char next = (unsigned char)text[i + 1u];
                next_is_printable_non_quote = (next != '\'') && (isprint(next) != 0);
            }
            if (!next_is_printable_non_quote) {
                if (!bx_printf_append_text(out, out_cap, &out_len, "''")) {
                    free(out);
                    return NULL;
                }
            }
            emitted = true;
            continue;
        }

        if (mode != BX_PRINTF_QUOTE_MODE_SINGLE) {
            if (!bx_printf_append_char(out, out_cap, &out_len, '\'')) {
                free(out);
                return NULL;
            }
            mode = BX_PRINTF_QUOTE_MODE_SINGLE;
            emitted = true;
        }

        if (!bx_printf_append_char(out, out_cap, &out_len, (char)ch)) {
            free(out);
            return NULL;
        }
        emitted = true;
    }

    if (mode == BX_PRINTF_QUOTE_MODE_SINGLE || mode == BX_PRINTF_QUOTE_MODE_DOLLAR) {
        if (!bx_printf_append_char(out, out_cap, &out_len, '\'')) {
            free(out);
            return NULL;
        }
    }

    if (!emitted) {
        if (!bx_printf_append_text(out, out_cap, &out_len, "''")) {
            free(out);
            return NULL;
        }
    }

    return out;
}

static bool bx_printf_decode_b_argument(const char* text, struct bx_printf_byte_buffer* buffer, bool* stop_output, struct bx_diag_ctx* diag) {
    const char* p = (text != NULL) ? text : "";
    *stop_output = false;

    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;
        if (ch != '\\') {
            if (!bx_printf_byte_buffer_append(buffer, &ch, 1)) {
                bx_diag(diag, "out of memory");
                return false;
            }
            continue;
        }

        unsigned char escaped[8];
        size_t escaped_len = 0;
        bool stop = false;
        if (!bx_printf_decode_escape(&p, true, escaped, &escaped_len, &stop, diag)) {
            return false;
        }
        if (stop) {
            *stop_output = true;
            return true;
        }

        if (!bx_printf_byte_buffer_append(buffer, escaped, escaped_len)) {
            bx_diag(diag, "out of memory");
            return false;
        }
    }

    return true;
}

#define BX_PRINTF_CALL_NO_VALUE(spec, width, precision)                                                                                                             \
    ((spec)->width_from_arg ? ((spec)->precision_from_arg ? fprintf(stdout, (spec)->canonical, (width), (precision)) : fprintf(stdout, (spec)->canonical, (width))) \
                            : ((spec)->precision_from_arg ? fprintf(stdout, (spec)->canonical, (precision)) : fprintf(stdout, (spec)->canonical)))

#define BX_PRINTF_CALL_VALUE(spec, width, precision, value)                                                                                                                           \
    ((spec)->width_from_arg ? ((spec)->precision_from_arg ? fprintf(stdout, (spec)->canonical, (width), (precision), (value)) : fprintf(stdout, (spec)->canonical, (width), (value))) \
                            : ((spec)->precision_from_arg ? fprintf(stdout, (spec)->canonical, (precision), (value)) : fprintf(stdout, (spec)->canonical, (value))))

static bool bx_printf_check_fprintf_result(int result, struct bx_diag_ctx* diag) {
    if (result < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_printf_render_b_conversion(const struct bx_printf_spec* spec, const char* value_arg, int runtime_width, int runtime_precision, bool* stop_output, struct bx_diag_ctx* diag) {
    struct bx_printf_byte_buffer decoded;
    bx_printf_byte_buffer_init(&decoded);

    bool stop_from_escape = false;
    bool ok = bx_printf_decode_b_argument(value_arg, &decoded, &stop_from_escape, diag);
    if (!ok) {
        bx_printf_byte_buffer_free(&decoded);
        return false;
    }

    size_t output_len = decoded.len;

    int precision = spec->has_precision ? runtime_precision : -1;
    if (precision >= 0 && output_len > (size_t)precision) {
        output_len = (size_t)precision;
    }

    bool left_adjust = bx_printf_spec_has_flag(spec, '-');
    int width = spec->has_width ? runtime_width : 0;
    if (width < 0) {
        left_adjust = true;
        if (width == INT_MIN) {
            width = INT_MAX;
        }
        else {
            width = -width;
        }
    }

    size_t pad_len = 0;
    if ((size_t)width > output_len) {
        pad_len = (size_t)width - output_len;
    }

    if (!left_adjust && !bx_printf_write_padding(diag, pad_len)) {
        bx_printf_byte_buffer_free(&decoded);
        return false;
    }

    if (!bx_printf_write_bytes(diag, decoded.data, output_len)) {
        bx_printf_byte_buffer_free(&decoded);
        return false;
    }

    if (left_adjust && !bx_printf_write_padding(diag, pad_len)) {
        bx_printf_byte_buffer_free(&decoded);
        return false;
    }

    bx_printf_byte_buffer_free(&decoded);
    *stop_output = stop_from_escape;
    return true;
}

static bool bx_printf_render_conversion(const struct bx_printf_spec* spec, const char* const* arguments, size_t argument_count, size_t base, bool* stop_output, struct bx_diag_ctx* diag) {
    *stop_output = false;

    int width = spec->width_value;
    int precision = spec->precision_value;

    if (spec->width_from_arg) {
        const char* arg = bx_printf_get_argument(arguments, argument_count, base, spec->width_position);
        width = bx_printf_parse_int_value(arg, diag);
    }

    if (spec->precision_from_arg) {
        const char* arg = bx_printf_get_argument(arguments, argument_count, base, spec->precision_position);
        precision = bx_printf_parse_int_value(arg, diag);
    }

    const char* value_arg = NULL;
    if (spec->value_consumes_argument) {
        value_arg = bx_printf_get_argument(arguments, argument_count, base, spec->value_position);
    }

    int result = 0;

    switch (spec->conversion) {
        case '%':
        case 'm':
            result = BX_PRINTF_CALL_NO_VALUE(spec, width, precision);
            return bx_printf_check_fprintf_result(result, diag);

        case 'b':
            return bx_printf_render_b_conversion(spec, value_arg, width, precision, stop_output, diag);

        case 'q': {
            const char* text = (value_arg != NULL) ? value_arg : "";
            char* quoted = bx_printf_quote_shell(text);
            if (quoted == NULL) {
                bx_diag(diag, "out of memory");
                return false;
            }

            bool ok = bx_printf_write_bytes(diag, (const unsigned char*)quoted, strlen(quoted));
            free(quoted);
            return ok;
        }

        case 'd':
        case 'i': {
            intmax_t signed_value = bx_printf_parse_signed_value(value_arg, diag);
            switch (spec->length) {
                case BX_PRINTF_LEN_HH:
                case BX_PRINTF_LEN_H:
                case BX_PRINTF_LEN_NONE:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (int)signed_value);
                    break;
                case BX_PRINTF_LEN_L:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (long)signed_value);
                    break;
                case BX_PRINTF_LEN_LL:
                case BX_PRINTF_LEN_Q:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (long long)signed_value);
                    break;
                case BX_PRINTF_LEN_J:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (intmax_t)signed_value);
                    break;
                case BX_PRINTF_LEN_Z:
                case BX_PRINTF_LEN_CAP_Z:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (ssize_t)signed_value);
                    break;
                case BX_PRINTF_LEN_T:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (ptrdiff_t)signed_value);
                    break;
                case BX_PRINTF_LEN_CAP_L:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (long long)signed_value);
                    break;
            }
            return bx_printf_check_fprintf_result(result, diag);
        }

        case 'o':
        case 'u':
        case 'x':
        case 'X': {
            uintmax_t unsigned_value = bx_printf_parse_unsigned_value(value_arg, diag);
            switch (spec->length) {
                case BX_PRINTF_LEN_HH:
                case BX_PRINTF_LEN_H:
                case BX_PRINTF_LEN_NONE:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (unsigned int)unsigned_value);
                    break;
                case BX_PRINTF_LEN_L:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (unsigned long)unsigned_value);
                    break;
                case BX_PRINTF_LEN_LL:
                case BX_PRINTF_LEN_Q:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (unsigned long long)unsigned_value);
                    break;
                case BX_PRINTF_LEN_J:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (uintmax_t)unsigned_value);
                    break;
                case BX_PRINTF_LEN_Z:
                case BX_PRINTF_LEN_CAP_Z:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (size_t)unsigned_value);
                    break;
                case BX_PRINTF_LEN_T:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (uintmax_t)unsigned_value);
                    break;
                case BX_PRINTF_LEN_CAP_L:
                    result = BX_PRINTF_CALL_VALUE(spec, width, precision, (unsigned long long)unsigned_value);
                    break;
            }
            return bx_printf_check_fprintf_result(result, diag);
        }

        case 'a':
        case 'A':
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G': {
            long double float_value = bx_printf_parse_float_value(value_arg, diag);
            if (spec->length == BX_PRINTF_LEN_CAP_L) {
                result = BX_PRINTF_CALL_VALUE(spec, width, precision, (long double)float_value);
            }
            else {
                result = BX_PRINTF_CALL_VALUE(spec, width, precision, (double)float_value);
            }
            return bx_printf_check_fprintf_result(result, diag);
        }

        case 'c': {
            unsigned char ch = '\0';
            if (value_arg != NULL && value_arg[0] != '\0') {
                ch = (unsigned char)value_arg[0];
            }
            result = BX_PRINTF_CALL_VALUE(spec, width, precision, (int)ch);
            return bx_printf_check_fprintf_result(result, diag);
        }

        case 's': {
            const char* text = (value_arg != NULL) ? value_arg : "";
            result = BX_PRINTF_CALL_VALUE(spec, width, precision, text);
            return bx_printf_check_fprintf_result(result, diag);
        }

        case 'p': {
            uintmax_t pointer_value = bx_printf_parse_unsigned_value(value_arg, diag);
            void* ptr = (void*)(uintptr_t)pointer_value;
            result = BX_PRINTF_CALL_VALUE(spec, width, precision, ptr);
            return bx_printf_check_fprintf_result(result, diag);
        }

        default:
            bx_diag(diag, "invalid conversion specification");
            return false;
    }
}

static bool bx_printf_render_cycle(const char* format, const char* const* arguments, size_t argument_count, size_t base, int* cycle_argument_count, bool* stop_output, struct bx_diag_ctx* diag) {
    const char* p = format;
    int next_auto_position = 1;
    int max_position = 0;

    *stop_output = false;

    while (*p != '\0') {
        unsigned char ch = (unsigned char)*p++;

        if (ch == '\\') {
            unsigned char escaped[8];
            size_t escaped_len = 0;
            bool stop_from_escape = false;
            if (!bx_printf_decode_escape(&p, true, escaped, &escaped_len, &stop_from_escape, diag)) {
                return false;
            }
            if (stop_from_escape) {
                *stop_output = true;
                break;
            }
            if (!bx_printf_write_bytes(diag, escaped, escaped_len)) {
                return false;
            }
            continue;
        }

        if (ch != '%') {
            if (!bx_printf_write_bytes(diag, &ch, 1)) {
                return false;
            }
            continue;
        }

        struct bx_printf_spec spec;
        if (!bx_printf_parse_spec(&p, &spec, &next_auto_position, &max_position, diag)) {
            return false;
        }

        bool stop_from_conversion = false;
        if (!bx_printf_render_conversion(&spec, arguments, argument_count, base, &stop_from_conversion, diag)) {
            return false;
        }
        if (stop_from_conversion) {
            *stop_output = true;
            break;
        }
    }

    *cycle_argument_count = max_position;
    return true;
}

int bx_printf_main(int argc, char** argv) {
    struct bx_printf_options options;
    struct bx_diag_ctx diag = {
        .progname = "printf",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_printf_parse_options(argc, argv, &options, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_printf_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_printf_print_version(options.progname);
        return 0;
    }

    const char* const* arguments = (const char* const*)(argv + options.first_argument);
    size_t argument_count = (size_t)(argc - options.first_argument);

    size_t base = 0;
    while (true) {
        int cycle_argument_count = 0;
        bool stop_output = false;

        if (!bx_printf_render_cycle(options.format, arguments, argument_count, base, &cycle_argument_count, &stop_output, &diag)) {
            return (diag.exit_status != 0) ? diag.exit_status : 1;
        }

        if (stop_output) {
            break;
        }

        if (cycle_argument_count <= 0) {
            break;
        }

        size_t cycle_args = (size_t)cycle_argument_count;
        if (base > SIZE_MAX - cycle_args) {
            break;
        }

        base += cycle_args;
        if (base >= argument_count) {
            break;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
