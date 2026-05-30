#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/args_common.h"
#include "lib/cli_common.h"
#include "lib/line_writer.h"

enum tr_array_kind {
    TR_ARRAY_STRING1 = 0,
    TR_ARRAY_STRING2,
};

enum tr_parse_attempt {
    TR_PARSE_NO_MATCH = 0,
    TR_PARSE_OK,
    TR_PARSE_ERROR,
};

struct tr_options {
    const char* progname;
    bool complement;
    bool delete_bytes;
    bool squeeze;
    bool truncate_set1;
    bool show_help;
    bool show_version;
};

struct tr_byte_array {
    unsigned char* data;
    size_t len;
    size_t cap;
};

static const char* tr_progname(const char* argv0) {
    return bx_cli_progname(argv0, "tr");
}

static void tr_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... STRING1 [STRING2]\n", progname);
    fprintf(stream, "Translate, squeeze, and/or delete characters from standard input,\n");
    fprintf(stream, "writing to standard output.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, -C, --complement      use the complement of ARRAY1\n");
    fprintf(stream, "  -d, --delete              delete characters in ARRAY1, do not translate\n");
    fprintf(stream, "  -s, --squeeze-repeats     replace each sequence of a repeated character\n");
    fprintf(stream, "                              listed in the last specified ARRAY with one\n");
    fprintf(stream, "                              occurrence of that character\n");
    fprintf(stream, "  -t, --truncate-set1       first truncate ARRAY1 to length of ARRAY2\n");
    fprintf(stream, "      --help                display this help and exit\n");
    fprintf(stream, "      --version             output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "ARRAYs are specified as strings of characters.  Interpreted sequences are:\n");
    fprintf(stream, "\n");
    fprintf(stream, "  \\\\NNN                     character with octal value NNN (1 to 3 digits)\n");
    fprintf(stream, "  \\\\\\\\                      backslash\n");
    fprintf(stream, "  \\\\a                       audible BEL\n");
    fprintf(stream, "  \\\\b                       backspace\n");
    fprintf(stream, "  \\\\f                       form feed\n");
    fprintf(stream, "  \\\\n                       new line\n");
    fprintf(stream, "  \\\\r                       carriage return\n");
    fprintf(stream, "  \\\\t                       horizontal tab\n");
    fprintf(stream, "  \\\\v                       vertical tab\n");
    fprintf(stream, "  CHAR1-CHAR2               all characters from CHAR1 to CHAR2\n");
    fprintf(stream, "  [CHAR*]                   in STRING2, copies of CHAR until length of STRING1\n");
    fprintf(stream, "  [CHAR*REPEAT]             REPEAT copies of CHAR; octal if starting with 0\n");
    fprintf(stream, "  [:CLASS:]                 character class expansion\n");
    fprintf(stream, "  [=CHAR=]                  all characters equivalent to CHAR\n");
}

static void tr_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void tr_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void tr_vdiag(const char* progname, const char* fmt, va_list ap) {
    fprintf(stderr, "%s: ", progname);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void tr_diag(const char* progname, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tr_vdiag(progname, fmt, ap);
    va_end(ap);
}

static int tr_usage_error(const char* progname, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    tr_vdiag(progname, fmt, ap);
    va_end(ap);
    tr_print_try_help(progname);
    return 1;
}

static void tr_array_destroy(struct tr_byte_array* array) {
    free(array->data);
    array->data = NULL;
    array->len = 0;
    array->cap = 0;
}

static void tr_array_reserve(struct tr_byte_array* array, size_t needed) {
    if (array->cap >= needed) {
        return;
    }

    size_t new_cap = (array->cap == 0u) ? 64u : array->cap;
    while (new_cap < needed) {
        new_cap *= 2u;
    }

    array->data = xrealloc(array->data, new_cap * sizeof(*array->data));
    array->cap = new_cap;
}

static void tr_array_append(struct tr_byte_array* array, unsigned char byte) {
    tr_array_reserve(array, array->len + 1u);
    array->data[array->len++] = byte;
}

static void tr_array_append_repeat(struct tr_byte_array* array, unsigned char byte, size_t count) {
    if (count == 0u) {
        return;
    }

    tr_array_reserve(array, array->len + count);
    memset(array->data + array->len, (int)byte, count);
    array->len += count;
}

static bool tr_is_octal_digit(char ch) {
    return ch >= '0' && ch <= '7';
}

static bool tr_parse_char_token(const char* text, size_t* consumed_out, unsigned char* byte_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    if (text[0] != '\\') {
        *consumed_out = 1u;
        *byte_out = (unsigned char)text[0];
        return true;
    }

    if (text[1] == '\0') {
        *consumed_out = 1u;
        *byte_out = (unsigned char)'\\';
        return true;
    }

    if (tr_is_octal_digit(text[1])) {
        unsigned int value = 0u;
        size_t consumed = 1u;
        while (consumed < 4u && tr_is_octal_digit(text[consumed])) {
            value = (value * 8u) + (unsigned int)(text[consumed] - '0');
            consumed++;
        }
        *consumed_out = consumed;
        *byte_out = (unsigned char)value;
        return true;
    }

    *consumed_out = 2u;
    switch (text[1]) {
        case 'a':
            *byte_out = '\a';
            return true;
        case 'b':
            *byte_out = '\b';
            return true;
        case 'f':
            *byte_out = '\f';
            return true;
        case 'n':
            *byte_out = '\n';
            return true;
        case 'r':
            *byte_out = '\r';
            return true;
        case 't':
            *byte_out = '\t';
            return true;
        case 'v':
            *byte_out = '\v';
            return true;
        case '\\':
            *byte_out = '\\';
            return true;
        default:
            *byte_out = (unsigned char)text[1];
            return true;
    }
}

static bool tr_byte_matches_class(unsigned char byte, const char* class_name) {
    int ch = (int)byte;

    if (strcmp(class_name, "alnum") == 0) {
        return isalnum(ch) != 0;
    }
    if (strcmp(class_name, "alpha") == 0) {
        return isalpha(ch) != 0;
    }
    if (strcmp(class_name, "blank") == 0) {
        return byte == ' ' || byte == '\t';
    }
    if (strcmp(class_name, "cntrl") == 0) {
        return iscntrl(ch) != 0;
    }
    if (strcmp(class_name, "digit") == 0) {
        return isdigit(ch) != 0;
    }
    if (strcmp(class_name, "graph") == 0) {
        return isgraph(ch) != 0;
    }
    if (strcmp(class_name, "lower") == 0) {
        return islower(ch) != 0;
    }
    if (strcmp(class_name, "print") == 0) {
        return isprint(ch) != 0;
    }
    if (strcmp(class_name, "punct") == 0) {
        return ispunct(ch) != 0;
    }
    if (strcmp(class_name, "space") == 0) {
        return isspace(ch) != 0;
    }
    if (strcmp(class_name, "upper") == 0) {
        return isupper(ch) != 0;
    }
    if (strcmp(class_name, "xdigit") == 0) {
        return isxdigit(ch) != 0;
    }

    return false;
}

static bool tr_append_char_class(const char* progname, struct tr_byte_array* array, const char* class_name) {
    static const char* const supported_classes[] = {
        "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit",
    };

    bool known = false;
    for (size_t i = 0; i < sizeof(supported_classes) / sizeof(supported_classes[0]); i++) {
        if (strcmp(class_name, supported_classes[i]) == 0) {
            known = true;
            break;
        }
    }

    if (!known) {
        tr_diag(progname, "invalid character class '%s'", class_name);
        return false;
    }

    for (int byte = 0; byte <= 255; byte++) {
        if (tr_byte_matches_class((unsigned char)byte, class_name)) {
            tr_array_append(array, (unsigned char)byte);
        }
    }

    return true;
}

static enum tr_parse_attempt tr_try_parse_char_class(const char* progname, const char* text, size_t* consumed_out, struct tr_byte_array* array) {
    if (text[0] != '[' || text[1] != ':') {
        return TR_PARSE_NO_MATCH;
    }

    const char* end = strstr(text + 2, ":]");
    if (end == NULL) {
        tr_diag(progname, "unterminated character class in '%s'", text);
        return TR_PARSE_ERROR;
    }

    size_t name_len = (size_t)(end - (text + 2));
    char* class_name = xmalloc(name_len + 1u);
    memcpy(class_name, text + 2, name_len);
    class_name[name_len] = '\0';

    bool ok = tr_append_char_class(progname, array, class_name);
    free(class_name);

    if (!ok) {
        return TR_PARSE_ERROR;
    }

    *consumed_out = name_len + 4u;
    return TR_PARSE_OK;
}

static enum tr_parse_attempt tr_try_parse_equivalence_class(const char* progname, const char* text, size_t* consumed_out, struct tr_byte_array* array) {
    if (text[0] != '[' || text[1] != '=') {
        return TR_PARSE_NO_MATCH;
    }

    size_t char_consumed = 0u;
    unsigned char byte = 0u;
    if (!tr_parse_char_token(text + 2, &char_consumed, &byte)) {
        tr_diag(progname, "invalid equivalence class in '%s'", text);
        return TR_PARSE_ERROR;
    }

    size_t pos = 2u + char_consumed;
    if (text[pos] != '=' || text[pos + 1] != ']') {
        tr_diag(progname, "unterminated equivalence class in '%s'", text);
        return TR_PARSE_ERROR;
    }

    tr_array_append(array, byte);
    *consumed_out = pos + 2u;
    return TR_PARSE_OK;
}

static bool tr_parse_repeat_count(const char* progname, const char* text, size_t len, size_t* count_out) {
    if (len == 0u) {
        tr_diag(progname, "empty repeat count");
        return false;
    }

    int base = (text[0] == '0') ? 8 : 10;
    size_t value = 0u;
    for (size_t i = 0; i < len; i++) {
        unsigned int digit = 0u;
        if (text[i] >= '0' && text[i] <= '9') {
            digit = (unsigned int)(text[i] - '0');
        }
        else {
            tr_diag(progname, "invalid repeat count '%.*s'", (int)len, text);
            return false;
        }

        if (base == 8 && digit >= 8u) {
            tr_diag(progname, "invalid repeat count '%.*s'", (int)len, text);
            return false;
        }

        value = (value * (size_t)base) + (size_t)digit;
    }

    *count_out = value;
    return true;
}

static enum tr_parse_attempt tr_try_parse_repeat(const char* progname, const char* text, size_t target_len, size_t* consumed_out, struct tr_byte_array* array) {
    if (text[0] != '[') {
        return TR_PARSE_NO_MATCH;
    }

    size_t char_consumed = 0u;
    unsigned char byte = 0u;
    if (!tr_parse_char_token(text + 1, &char_consumed, &byte)) {
        return TR_PARSE_NO_MATCH;
    }

    size_t pos = 1u + char_consumed;
    if (text[pos] != '*') {
        return TR_PARSE_NO_MATCH;
    }

    pos++;
    size_t repeat_count = 0u;
    if (text[pos] == ']') {
        repeat_count = (target_len > array->len) ? (target_len - array->len) : 0u;
        *consumed_out = pos + 1u;
        tr_array_append_repeat(array, byte, repeat_count);
        return TR_PARSE_OK;
    }

    const char* count_start = text + pos;
    while (text[pos] != '\0' && text[pos] != ']') {
        pos++;
    }

    if (text[pos] != ']') {
        tr_diag(progname, "unterminated repeat specification in '%s'", text);
        return TR_PARSE_ERROR;
    }

    if (!tr_parse_repeat_count(progname, count_start, (size_t)(text + pos - count_start), &repeat_count)) {
        return TR_PARSE_ERROR;
    }

    tr_array_append_repeat(array, byte, repeat_count);
    *consumed_out = pos + 1u;
    return TR_PARSE_OK;
}

static bool tr_parse_array(const char* progname, const char* text, enum tr_array_kind kind, size_t repeat_target_len, struct tr_byte_array* array) {
    size_t pos = 0u;

    while (text[pos] != '\0') {
        size_t consumed = 0u;
        enum tr_parse_attempt parsed = TR_PARSE_NO_MATCH;

        if (text[pos] == '[' && text[pos + 1] == ':') {
            parsed = tr_try_parse_char_class(progname, text + pos, &consumed, array);
        }
        else if (text[pos] == '[' && text[pos + 1] == '=') {
            parsed = tr_try_parse_equivalence_class(progname, text + pos, &consumed, array);
        }
        else if (kind == TR_ARRAY_STRING2 && text[pos] == '[') {
            parsed = tr_try_parse_repeat(progname, text + pos, repeat_target_len, &consumed, array);
        }

        if (parsed == TR_PARSE_ERROR) {
            return false;
        }
        if (parsed == TR_PARSE_OK) {
            pos += consumed;
            continue;
        }

        size_t left_consumed = 0u;
        unsigned char left = 0u;
        if (!tr_parse_char_token(text + pos, &left_consumed, &left)) {
            tr_diag(progname, "invalid array specification near '%s'", text + pos);
            return false;
        }

        if (text[pos + left_consumed] == '-' && text[pos + left_consumed + 1] != '\0' && text[pos + left_consumed + 1] != '[') {
            size_t right_consumed = 0u;
            unsigned char right = 0u;
            if (!tr_parse_char_token(text + pos + left_consumed + 1u, &right_consumed, &right)) {
                tr_diag(progname, "invalid range end in '%s'", text + pos);
                return false;
            }

            if (left > right) {
                tr_diag(progname, "range-endpoints of '%c-%c' are in reverse collating sequence order", left, right);
                return false;
            }

            for (unsigned int byte = (unsigned int)left; byte <= (unsigned int)right; byte++) {
                tr_array_append(array, (unsigned char)byte);
            }

            pos += left_consumed + 1u + right_consumed;
            continue;
        }

        tr_array_append(array, left);
        pos += left_consumed;
    }

    return true;
}

static void tr_build_membership(const struct tr_byte_array* array, bool present[256]) {
    memset(present, 0, 256u * sizeof(present[0]));
    for (size_t i = 0; i < array->len; i++) {
        present[array->data[i]] = true;
    }
}

static void tr_append_complement(const bool present[256], struct tr_byte_array* array) {
    for (int byte = 0; byte <= 255; byte++) {
        if (!present[byte]) {
            tr_array_append(array, (unsigned char)byte);
        }
    }
}

int bx_tr_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"complement", no_argument, NULL, 'c'},
        {"delete", no_argument, NULL, 'd'},
        {"squeeze-repeats", no_argument, NULL, 's'},
        {"truncate-set1", no_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {NULL, 0, NULL, 0},
    };

    struct tr_options options = {
        .progname = tr_progname((argc > 0) ? argv[0] : NULL),
    };

    bx_args_getopt_reset();
    int opt = 0;
    while ((opt = bx_args_getopt_long(argc, argv, "cCdst", long_options, NULL)) != -1) {
        switch (opt) {
            case 'c':
            case 'C':
                options.complement = true;
                break;
            case 'd':
                options.delete_bytes = true;
                break;
            case 's':
                options.squeeze = true;
                break;
            case 't':
                options.truncate_set1 = true;
                break;
            case 'h':
                options.show_help = true;
                break;
            case 'v':
                options.show_version = true;
                break;
            case '?':
                if (optind > 0 && optind - 1 < argc && argv[optind - 1] != NULL) {
                    return tr_usage_error(options.progname, "unrecognized option '%s'", argv[optind - 1]);
                }
                return tr_usage_error(options.progname, "unrecognized option");
            default:
                return 1;
        }
    }

    if (options.show_help) {
        tr_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        tr_print_version(options.progname);
        return 0;
    }

    int remaining = argc - optind;
    if (remaining <= 0) {
        return tr_usage_error(options.progname, "missing operand");
    }

    if (options.delete_bytes && !options.squeeze) {
        if (remaining > 1) {
            return tr_usage_error(options.progname, "Only one string may be given when deleting without squeezing repeats.");
        }
    }
    else if (!options.delete_bytes && !options.squeeze && remaining < 2) {
        return tr_usage_error(options.progname, "missing operand after '%s'", argv[optind]);
    }

    if (remaining > 2) {
        return tr_usage_error(options.progname, "extra operand '%s'", argv[optind + 2]);
    }

    struct tr_byte_array set1 = {0};
    struct tr_byte_array set2 = {0};
    struct tr_byte_array complemented = {0};

    bool ok = tr_parse_array(options.progname, argv[optind], TR_ARRAY_STRING1, 0u, &set1);
    if (!ok) {
        tr_array_destroy(&set1);
        return 1;
    }

    if (options.complement) {
        bool present[256];
        tr_build_membership(&set1, present);
        tr_append_complement(present, &complemented);
        tr_array_destroy(&set1);
        set1 = complemented;
        complemented.data = NULL;
        complemented.len = 0u;
        complemented.cap = 0u;
    }

    if (remaining >= 2) {
        ok = tr_parse_array(options.progname, argv[optind + 1], TR_ARRAY_STRING2, set1.len, &set2);
        if (!ok) {
            tr_array_destroy(&set1);
            tr_array_destroy(&set2);
            return 1;
        }
    }

    if (!options.delete_bytes && remaining >= 2 && set2.len == 0u && !options.truncate_set1) {
        tr_array_destroy(&set1);
        tr_array_destroy(&set2);
        tr_diag(options.progname, "when not truncating set1, string2 must be non-empty");
        return 1;
    }

    if (!options.delete_bytes && options.truncate_set1 && set2.len < set1.len) {
        set1.len = set2.len;
    }

    unsigned char map[256];
    bool delete_set[256];
    bool squeeze_set[256];

    for (int byte = 0; byte <= 255; byte++) {
        map[byte] = (unsigned char)byte;
    }
    memset(delete_set, 0, sizeof(delete_set));
    memset(squeeze_set, 0, sizeof(squeeze_set));

    if (options.delete_bytes) {
        for (size_t i = 0; i < set1.len; i++) {
            delete_set[set1.data[i]] = true;
        }
    }
    else if (remaining >= 2 && set1.len > 0u) {
        unsigned char last = (set2.len > 0u) ? set2.data[set2.len - 1u] : 0u;
        for (size_t i = 0; i < set1.len; i++) {
            unsigned char replacement = (i < set2.len) ? set2.data[i] : last;
            map[set1.data[i]] = replacement;
        }
    }

    if (options.squeeze) {
        const struct tr_byte_array* squeeze_array = NULL;
        if (remaining >= 2) {
            squeeze_array = &set2;
        }
        else {
            squeeze_array = &set1;
        }

        for (size_t i = 0; i < squeeze_array->len; i++) {
            squeeze_set[squeeze_array->data[i]] = true;
        }
    }

    int prev_out = -1;
    int ch = 0;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    while ((ch = getchar()) != EOF) {
        unsigned char byte = (unsigned char)ch;
        if (delete_set[byte]) {
            continue;
        }

        unsigned char out = map[byte];
        if (options.squeeze && prev_out == (int)out && squeeze_set[out]) {
            continue;
        }

        if (!bx_line_writer_putc(&writer, (char)out)) {
            tr_array_destroy(&set1);
            tr_array_destroy(&set2);
            return 1;
        }

        prev_out = (int)out;
    }

    tr_array_destroy(&set1);
    tr_array_destroy(&set2);

    bool output_error = bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer);
    if (ferror(stdin) || output_error) {
        return 1;
    }

    return 0;
}
