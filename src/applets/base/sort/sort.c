#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/size_parse.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"
#include "lib/random_bytes.h"
#include "crypto/sha256.h"

typedef enum {
    SORT_MODE_LEXICOGRAPHIC,
    SORT_MODE_NUMERIC,
    SORT_MODE_HUMAN,
    SORT_MODE_VERSION,
    SORT_MODE_RANDOM,
} sort_mode_t;

typedef struct {
    size_t field;
    size_t char_offset;
    bool has_char_offset;
    bool ignore_leading_blanks;
} sort_key_part_t;

typedef struct {
    sort_key_part_t start;
    sort_key_part_t end;
    bool has_end;
    sort_mode_t mode;
    bool has_mode;
    bool reverse;
    bool ignore_case;
} sort_key_spec_t;

typedef struct {
    sort_key_spec_t* items;
    size_t len;
    size_t cap;
} sort_key_spec_vec_t;

typedef struct {
    sort_mode_t mode;
    bool reverse;
    bool unique;
    bool ignore_case;
    bool ignore_leading_blanks;
    bool check;
    bool check_quiet;
    bool stable;
    bool zero_terminated;
    bool has_field_separator;
    unsigned char field_separator;
    sort_key_spec_vec_t key_specs;
    const char* output_path;
    const char* random_source;
    struct bx_sha256_ctx random_prefix;
} sort_opts_t;

typedef struct {
    char* text;
    char** key_texts;
    size_t original_index;
} sort_line_t;

typedef struct {
    sort_line_t* items;
    size_t len;
    size_t cap;
} sort_line_vec_t;

typedef struct {
    int sign;
    const unsigned char* int_digits;
    size_t int_len;
    const unsigned char* frac_digits;
    size_t frac_len;
    unsigned char suffix;
    bool has_digits;
    bool is_zero;
} numeric_token_t;

typedef struct {
    const char* base_start;
    const char* data_start;
    const char* data_end;
} sort_field_range_t;

typedef struct {
    sort_mode_t mode;
    bool ignore_case;
    bool reverse;
} sort_order_opts_t;

typedef enum {
    SORT_STREAM_OK,
    SORT_STREAM_ERROR,
    SORT_STREAM_NOMEM,
    SORT_STREAM_DISORDER,
} sort_stream_status_t;

enum {
    SORT_OPT_HELP = 256,
    SORT_OPT_VERSION,
    SORT_OPT_RANDOM_SOURCE,
};

static int sort_cmp_sign(int value) {
    if (value < 0)
        return -1;
    if (value > 0)
        return 1;
    return 0;
}

static void sort_report_errno(const char* progname, const char* path) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, bx_strerror(errno));
}

static void sort_report_memory_exhausted(const char* progname) {
    fprintf(stderr, "%s: memory exhausted\n", progname);
}

static void sort_print_help(const char* progname) {
    printf("Usage: %s [OPTION]... [FILE]...\n", progname);
    puts("Write sorted concatenation of all FILE(s) to standard output.");
    puts("");
    puts("Ordering options:");
    puts("  -b, --ignore-leading-blanks");
    puts("                          ignore leading blanks when finding sort keys");
    puts("  -n, --numeric-sort      compare according to numeric value");
    puts("  -h, --human-numeric-sort  compare human readable numbers (e.g., 2K 1G)");
    puts("  -V, --version-sort      natural sort of version numbers within text");
    puts("  -R, --random-sort       randomize key order, grouping equal keys");
    puts("      --random-source=FILE  read the random seed from FILE");
    puts("  -f, --ignore-case       fold lower case to upper case characters");
    puts("  -r, --reverse           reverse the result of comparisons");
    puts("  -s, --stable            stabilize sort by disabling last-resort comparison");
    puts("  -u, --unique            with -c, check for strict ordering; otherwise output");
    puts("                          only the first of equal keys");
    puts("");
    puts("Operation modes:");
    puts("  -c, --check             check for sorted input; do not sort");
    puts("  -C                      like -c, but do not report first bad line");
    puts("");
    puts("Key options:");
    puts("  -k, --key=POS1[,POS2]   start a key at POS1, end it at POS2");
    puts("  -t, --field-separator=SEP  use SEP instead of non-blank to blank transition");
    puts("");
    puts("Output control:");
    puts("  -o, --output=FILE       write result to FILE instead of standard output");
    puts("  -z, --zero-terminated   line delimiter is NUL, not newline");
    puts("");
    puts("      --help              display this help and exit");
    puts("      --version           output version information and exit");
}

static bool sort_key_spec_vec_push(sort_key_spec_vec_t* vec, const sort_key_spec_t* spec) {
    if (vec->len == vec->cap) {
        size_t new_cap = (vec->cap == 0) ? 4u : vec->cap * 2u;
        if (new_cap < vec->cap || new_cap > SIZE_MAX / sizeof(vec->items[0])) {
            errno = ENOMEM;
            return false;
        }
        sort_key_spec_t* resized = realloc(vec->items, new_cap * sizeof(vec->items[0]));
        if (!resized) {
            return false;
        }
        vec->items = resized;
        vec->cap = new_cap;
    }

    vec->items[vec->len] = *spec;
    vec->len++;
    return true;
}

static void sort_key_spec_vec_free(sort_key_spec_vec_t* vec) {
    free(vec->items);
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static void sort_free_key_texts(char** key_texts, size_t key_count) {
    if (!key_texts) {
        return;
    }
    for (size_t i = 0; i < key_count; i++) {
        free(key_texts[i]);
    }
    free(key_texts);
}

static bool sort_line_vec_push(sort_line_vec_t* vec, char* text, char** key_texts, size_t original_index) {
    if (vec->len == vec->cap) {
        size_t new_cap = (vec->cap == 0) ? 1024u : vec->cap * 2u;
        if (new_cap < vec->cap || new_cap > SIZE_MAX / sizeof(vec->items[0])) {
            errno = ENOMEM;
            return false;
        }
        sort_line_t* resized = realloc(vec->items, new_cap * sizeof(vec->items[0]));
        if (!resized) {
            return false;
        }
        vec->items = resized;
        vec->cap = new_cap;
    }

    vec->items[vec->len].text = text;
    vec->items[vec->len].key_texts = key_texts;
    vec->items[vec->len].original_index = original_index;
    vec->len++;
    return true;
}

static void sort_line_vec_free(sort_line_vec_t* vec, size_t key_count) {
    if (!vec->items) {
        return;
    }
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].text);
        sort_free_key_texts(vec->items[i].key_texts, key_count);
    }
    free(vec->items);
    vec->items = NULL;
    vec->len = 0;
    vec->cap = 0;
}

static bool sort_parse_positive_index(const char** text, size_t* value_out) {
    const unsigned char* p = (const unsigned char*)*text;
    if (!isdigit(*p)) {
        return false;
    }

    size_t value = 0;
    while (isdigit(*p)) {
        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (SIZE_MAX - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
        p++;
    }

    if (value == 0) {
        return false;
    }

    *text = (const char*)p;
    *value_out = value;
    return true;
}

static bool sort_set_mode(sort_mode_t* mode_out, bool* has_mode_out, sort_mode_t mode) {
    if (*has_mode_out && *mode_out != mode) {
        return false;
    }
    *mode_out = mode;
    *has_mode_out = true;
    return true;
}

static bool sort_parse_key_part(const char** text, sort_key_spec_t* spec, sort_key_part_t* part) {
    if (!sort_parse_positive_index(text, &part->field)) {
        return false;
    }

    if (**text == '.') {
        size_t char_number = 0;
        (*text)++;
        if (!sort_parse_positive_index(text, &char_number)) {
            return false;
        }
        part->has_char_offset = true;
        part->char_offset = char_number - 1u;
    }

    while (**text != '\0' && **text != ',') {
        switch (**text) {
            case 'b':
                part->ignore_leading_blanks = true;
                break;
            case 'f':
                spec->ignore_case = true;
                break;
            case 'r':
                spec->reverse = true;
                break;
            case 'n':
                if (!sort_set_mode(&spec->mode, &spec->has_mode, SORT_MODE_NUMERIC)) {
                    return false;
                }
                break;
            case 'h':
                if (!sort_set_mode(&spec->mode, &spec->has_mode, SORT_MODE_HUMAN)) {
                    return false;
                }
                break;
            case 'V':
                if (!sort_set_mode(&spec->mode, &spec->has_mode, SORT_MODE_VERSION)) {
                    return false;
                }
                break;
            case 'R':
                if (!sort_set_mode(&spec->mode, &spec->has_mode, SORT_MODE_RANDOM)) {
                    return false;
                }
                break;
            default:
                return false;
        }
        (*text)++;
    }

    return true;
}

static bool sort_parse_key_spec(const char* text, sort_key_spec_t* spec_out) {
    const char* p = text;
    sort_key_spec_t spec = {0};

    if (!sort_parse_key_part(&p, &spec, &spec.start)) {
        return false;
    }

    if (*p == ',') {
        p++;
        spec.has_end = true;
        if (!sort_parse_key_part(&p, &spec, &spec.end)) {
            return false;
        }
    } else if (*p != '\0') {
        return false;
    }

    *spec_out = spec;
    return true;
}

static bool sort_find_field_range(const char* line, const sort_opts_t* opts, size_t field_number, sort_field_range_t* range_out) {
    const char* line_end = line + strlen(line);

    if (opts->has_field_separator) {
        const char* cursor = line;
        size_t current = 1;
        while (current < field_number) {
            const char* separator = strchr(cursor, (int)opts->field_separator);
            if (!separator) {
                range_out->base_start = line_end;
                range_out->data_start = line_end;
                range_out->data_end = line_end;
                return false;
            }
            cursor = separator + 1;
            current++;
        }

        const char* separator = strchr(cursor, (int)opts->field_separator);
        range_out->base_start = cursor;
        range_out->data_start = cursor;
        range_out->data_end = separator ? separator : line_end;
        return true;
    }

    const char* cursor = line;
    for (size_t current = 1; current <= field_number; current++) {
        const char* base_start = cursor;
        while (*cursor != '\0' && isblank((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            range_out->base_start = line_end;
            range_out->data_start = line_end;
            range_out->data_end = line_end;
            return false;
        }

        const char* data_start = cursor;
        while (*cursor != '\0' && !isblank((unsigned char)*cursor)) {
            cursor++;
        }

        if (current == field_number) {
            range_out->base_start = base_start;
            range_out->data_start = data_start;
            range_out->data_end = cursor;
            return true;
        }
    }

    range_out->base_start = line_end;
    range_out->data_start = line_end;
    range_out->data_end = line_end;
    return false;
}

static char* sort_strdup_range(const char* start, size_t length) {
    if (length == SIZE_MAX) {
        errno = ENOMEM;
        return NULL;
    }

    char* copy = malloc(length + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, start, length);
    copy[length] = '\0';
    return copy;
}

static const char* sort_skip_leading_blanks(const char* text) {
    while (*text != '\0' && isblank((unsigned char)*text)) {
        text++;
    }
    return text;
}

static const char* sort_clamped_advance(const char* start, const char* end, size_t count) {
    size_t available = (size_t)(end - start);
    if (count > available) {
        return end;
    }
    return start + count;
}

static const char* sort_key_part_base(const sort_opts_t* opts, const sort_key_part_t* part, const sort_field_range_t* field) {
    if (opts->has_field_separator) {
        return field->data_start;
    }
    if (opts->ignore_leading_blanks || part->ignore_leading_blanks) {
        return field->data_start;
    }
    return field->base_start;
}

static char* sort_make_key_copy_for_spec(const sort_opts_t* opts, const sort_key_spec_t* spec, const char* line) {
    const char* line_end = line + strlen(line);
    const char* key_start = line_end;
    const char* key_end = line_end;

    sort_field_range_t start_field = {0};
    if (sort_find_field_range(line, opts, spec->start.field, &start_field)) {
        const char* start_base = sort_key_part_base(opts, &spec->start, &start_field);
        size_t start_offset = spec->start.has_char_offset ? spec->start.char_offset : 0u;
        key_start = sort_clamped_advance(start_base, start_field.data_end, start_offset);

        if (!spec->has_end) {
            key_end = line_end;
        }
        else {
            sort_field_range_t end_field = {0};
            if (!sort_find_field_range(line, opts, spec->end.field, &end_field)) {
                key_end = line_end;
            }
            else {
                if (spec->end.has_char_offset) {
                    const char* end_base = sort_key_part_base(opts, &spec->end, &end_field);
                    key_end = sort_clamped_advance(end_base, end_field.data_end, spec->end.char_offset + 1u);
                }
                else {
                    key_end = end_field.data_end;
                }
            }
        }
    }

    if (key_end < key_start) {
        key_end = key_start;
    }

    return sort_strdup_range(key_start, (size_t)(key_end - key_start));
}

static size_t sort_cached_key_count(const sort_opts_t* opts) {
    return opts->key_specs.len ? opts->key_specs.len : (opts->mode == SORT_MODE_RANDOM ? 1u : 0u);
}

static bool sort_seed_random_order(sort_opts_t* opts, const char* progname) {
    bool needed = opts->key_specs.len == 0 && opts->mode == SORT_MODE_RANDOM;
    for (size_t i = 0; i < opts->key_specs.len; i++) {
        const sort_key_spec_t* spec = &opts->key_specs.items[i];
        needed |= (spec->has_mode ? spec->mode : opts->mode) == SORT_MODE_RANDOM;
    }
    if (!needed) {
        return true;
    }

    unsigned char seed[BX_SHA256_DIGEST_SIZE];
    if (opts->random_source) {
        FILE* source = fopen(opts->random_source, "rb");
        if (!source) {
            sort_report_errno(progname, opts->random_source);
            return false;
        }
        size_t count = fread(seed, 1, sizeof(seed), source);
        int saved_errno = ferror(source) ? errno : EIO;
        int close_status = fclose(source);
        if (count != sizeof(seed) || close_status != 0) {
            if (count != sizeof(seed)) {
                errno = saved_errno;
            }
            sort_report_errno(progname, opts->random_source);
            return false;
        }
    } else if (!bx_random_bytes(seed, sizeof(seed))) {
        sort_report_errno(progname, "random seed");
        return false;
    }
    bx_sha256_init(&opts->random_prefix);
    bx_sha256_update(&opts->random_prefix, seed, sizeof(seed));
    return true;
}

static char* sort_make_random_key(const sort_opts_t* opts, char* text, bool ignore_case) {
    if (ignore_case) {
        for (unsigned char* p = (unsigned char*)text; *p; p++) {
            *p = (unsigned char)toupper(*p);
        }
    }
    char unused;
    size_t length = strxfrm(&unused, text, 0);
    if (length > SIZE_MAX - BX_SHA256_DIGEST_SIZE - 1u) {
        errno = ENOMEM;
        return NULL;
    }
    char* key = malloc(BX_SHA256_DIGEST_SIZE + length + 1u);
    if (!key) {
        return NULL;
    }
    /* Cache the digest and collation key; equal keys must remain grouped. */
    strxfrm(key + BX_SHA256_DIGEST_SIZE, text, length + 1u);
    struct bx_sha256_ctx hash = opts->random_prefix;
    bx_sha256_update(&hash, key + BX_SHA256_DIGEST_SIZE, length);
    bx_sha256_final(&hash, (unsigned char*)key);
    return key;
}

static char** sort_make_key_copies(const sort_opts_t* opts, const char* line) {
    size_t count = sort_cached_key_count(opts);
    if (count == 0) {
        return NULL;
    }

    char** key_texts = calloc(count, sizeof(key_texts[0]));
    if (!key_texts) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        const sort_key_spec_t* spec = opts->key_specs.len ? &opts->key_specs.items[i] : NULL;
        sort_mode_t mode = spec && spec->has_mode ? spec->mode : opts->mode;
        key_texts[i] = spec ? sort_make_key_copy_for_spec(opts, spec, line)
                           : strdup(opts->ignore_leading_blanks ? sort_skip_leading_blanks(line) : line);
        if (key_texts[i] && mode == SORT_MODE_RANDOM) {
            char* key = sort_make_random_key(opts, key_texts[i], opts->ignore_case || (spec && spec->ignore_case));
            free(key_texts[i]);
            key_texts[i] = key;
        }
        if (!key_texts[i]) {
            sort_free_key_texts(key_texts, count);
            return NULL;
        }
    }

    return key_texts;
}

static void sort_parse_numeric_token(const char* text, numeric_token_t* token) {
    const unsigned char* p = (const unsigned char*)text;

    while (isspace(*p)) {
        p++;
    }

    token->sign = 1;
    if (*p == '-' || *p == '+') {
        if (*p == '-') {
            token->sign = -1;
        }
        p++;
    }

    const unsigned char* int_begin = p;
    while (isdigit(*p)) {
        p++;
    }
    const unsigned char* int_end = p;

    const unsigned char* frac_begin = p;
    const unsigned char* frac_end = p;
    if (*p == '.') {
        p++;
        frac_begin = p;
        while (isdigit(*p)) {
            p++;
        }
        frac_end = p;
    }

    token->int_digits = int_begin;
    token->int_len = (size_t)(int_end - int_begin);
    token->frac_digits = frac_begin;
    token->frac_len = (size_t)(frac_end - frac_begin);
    token->suffix = *p;
    token->has_digits = (token->int_len > 0 || token->frac_len > 0);

    token->is_zero = true;
    if (token->has_digits) {
        for (size_t i = 0; i < token->int_len; i++) {
            if (token->int_digits[i] != '0') {
                token->is_zero = false;
                break;
            }
        }
        if (token->is_zero) {
            for (size_t i = 0; i < token->frac_len; i++) {
                if (token->frac_digits[i] != '0') {
                    token->is_zero = false;
                    break;
                }
            }
        }
    }

    if (!token->has_digits || token->is_zero) {
        token->sign = 1;
    }
}

static void sort_parse_human_token(const char* text, numeric_token_t* token) {
    const unsigned char* p = (const unsigned char*)text;

    while (isspace(*p)) {
        p++;
    }

    token->sign = 1;
    if (*p == '-') {
        token->sign = -1;
        p++;
    }
    else if (*p == '+') {
        token->int_digits = p;
        token->int_len = 0u;
        token->frac_digits = p;
        token->frac_len = 0u;
        token->suffix = *p;
        token->has_digits = false;
        token->is_zero = true;
        return;
    }

    const unsigned char* int_begin = p;
    while (isdigit(*p)) {
        p++;
    }
    const unsigned char* int_end = p;

    const unsigned char* frac_begin = p;
    const unsigned char* frac_end = p;
    if (*p == '.') {
        p++;
        frac_begin = p;
        while (isdigit(*p)) {
            p++;
        }
        frac_end = p;
    }

    size_t int_len = (size_t)(int_end - int_begin);
    size_t frac_len = (size_t)(frac_end - frac_begin);
    token->int_digits = int_begin;
    token->int_len = int_len;
    token->frac_digits = frac_begin;
    token->frac_len = frac_len;
    token->suffix = *p;
    token->has_digits = (int_len > 0u || frac_len > 0u);
    token->is_zero = true;

    if (token->has_digits) {
        for (size_t i = 0; i < token->int_len; i++) {
            if (token->int_digits[i] != '0') {
                token->is_zero = false;
                break;
            }
        }
        if (token->is_zero) {
            for (size_t i = 0; i < token->frac_len; i++) {
                if (token->frac_digits[i] != '0') {
                    token->is_zero = false;
                    break;
                }
            }
        }
    }

    if (!token->has_digits || token->is_zero) {
        token->sign = 1;
    }
}

static int sort_compare_numeric_abs(const numeric_token_t* left, const numeric_token_t* right) {
    const unsigned char* left_int = left->int_digits;
    size_t left_int_len = left->int_len;
    while (left_int_len > 0 && *left_int == '0') {
        left_int++;
        left_int_len--;
    }

    const unsigned char* right_int = right->int_digits;
    size_t right_int_len = right->int_len;
    while (right_int_len > 0 && *right_int == '0') {
        right_int++;
        right_int_len--;
    }

    if (left_int_len != right_int_len) {
        return (left_int_len < right_int_len) ? -1 : 1;
    }

    if (left_int_len > 0) {
        int int_cmp = memcmp(left_int, right_int, left_int_len);
        if (int_cmp != 0) {
            return sort_cmp_sign(int_cmp);
        }
    }

    const unsigned char* left_frac = left->frac_digits;
    size_t left_frac_len = left->frac_len;
    while (left_frac_len > 0 && left_frac[left_frac_len - 1] == '0') {
        left_frac_len--;
    }

    const unsigned char* right_frac = right->frac_digits;
    size_t right_frac_len = right->frac_len;
    while (right_frac_len > 0 && right_frac[right_frac_len - 1] == '0') {
        right_frac_len--;
    }

    size_t max_frac_len = (left_frac_len > right_frac_len) ? left_frac_len : right_frac_len;
    for (size_t i = 0; i < max_frac_len; i++) {
        unsigned char left_digit = (i < left_frac_len) ? left_frac[i] : '0';
        unsigned char right_digit = (i < right_frac_len) ? right_frac[i] : '0';
        if (left_digit < right_digit) {
            return -1;
        }
        if (left_digit > right_digit) {
            return 1;
        }
    }

    return 0;
}

static int sort_compare_numeric_key(const char* left, const char* right) {
    numeric_token_t left_token = {0};
    numeric_token_t right_token = {0};
    sort_parse_numeric_token(left, &left_token);
    sort_parse_numeric_token(right, &right_token);

    if (left_token.is_zero && right_token.is_zero) {
        return 0;
    }

    if (left_token.sign != right_token.sign) {
        return (left_token.sign < right_token.sign) ? -1 : 1;
    }

    int abs_cmp = sort_compare_numeric_abs(&left_token, &right_token);
    if (left_token.sign < 0) {
        abs_cmp = -abs_cmp;
    }
    return abs_cmp;
}

static unsigned int sort_human_suffix_magnitude(unsigned char suffix) {
    unsigned int power = 0u;
    if (!bx_size_suffix_prefix_power((char)suffix, &power)) {
        return 0u;
    }
    return power;
}

static unsigned int sort_human_suffix_variant(unsigned char suffix) {
    if (suffix == 'k') {
        return 1u;
    }
    return 0u;
}

static int sort_compare_human_key(const char* left, const char* right) {
    numeric_token_t left_token = {0};
    numeric_token_t right_token = {0};
    sort_parse_human_token(left, &left_token);
    sort_parse_human_token(right, &right_token);

    if (left_token.sign != right_token.sign) {
        return (left_token.sign < right_token.sign) ? -1 : 1;
    }

    if (left_token.is_zero || right_token.is_zero) {
        if (left_token.is_zero && right_token.is_zero) {
            return 0;
        }
        return left_token.is_zero ? -1 : 1;
    }

    unsigned int left_magnitude = left_token.has_digits ? sort_human_suffix_magnitude(left_token.suffix) : 0u;
    unsigned int right_magnitude = right_token.has_digits ? sort_human_suffix_magnitude(right_token.suffix) : 0u;
    if (left_magnitude != right_magnitude) {
        if (left_token.sign < 0) {
            return (left_magnitude < right_magnitude) ? 1 : -1;
        }
        return (left_magnitude < right_magnitude) ? -1 : 1;
    }

    int abs_cmp = sort_compare_numeric_abs(&left_token, &right_token);
    if (left_token.sign < 0) {
        abs_cmp = -abs_cmp;
    }
    if (abs_cmp != 0) {
        return abs_cmp;
    }

    unsigned int left_variant = left_token.has_digits ? sort_human_suffix_variant(left_token.suffix) : 0u;
    unsigned int right_variant = right_token.has_digits ? sort_human_suffix_variant(right_token.suffix) : 0u;
    if (left_variant != right_variant) {
        return (left_variant < right_variant) ? -1 : 1;
    }

    return 0;
}

static int sort_compare_char(unsigned char left, unsigned char right, bool ignore_case) {
    if (ignore_case) {
        left = (unsigned char)tolower(left);
        right = (unsigned char)tolower(right);
    }
    if (left < right) {
        return -1;
    }
    if (left > right) {
        return 1;
    }
    return 0;
}

static int sort_compare_version_key(const char* left, const char* right, bool ignore_case) {
    const unsigned char* a = (const unsigned char*)left;
    const unsigned char* b = (const unsigned char*)right;

    while (*a != '\0' || *b != '\0') {
        if (isdigit(*a) && isdigit(*b)) {
            const unsigned char* a_run = a;
            const unsigned char* b_run = b;

            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }

            const unsigned char* a_digits = a;
            const unsigned char* b_digits = b;
            while (isdigit(*a)) {
                a++;
            }
            while (isdigit(*b)) {
                b++;
            }

            size_t a_len = (size_t)(a - a_digits);
            size_t b_len = (size_t)(b - b_digits);
            if (a_len != b_len) {
                return (a_len < b_len) ? -1 : 1;
            }

            if (a_len > 0) {
                int digit_cmp = memcmp(a_digits, b_digits, a_len);
                if (digit_cmp != 0) {
                    return sort_cmp_sign(digit_cmp);
                }
            }

            size_t a_run_len = (size_t)(a - a_run);
            size_t b_run_len = (size_t)(b - b_run);
            if (a_run_len != b_run_len) {
                return (a_run_len < b_run_len) ? 1 : -1;
            }
            continue;
        }

        int char_cmp = sort_compare_char(*a, *b, ignore_case);
        if (char_cmp != 0) {
            return char_cmp;
        }
        if (*a == '\0') {
            return 0;
        }

        a++;
        b++;
    }

    return 0;
}

static int sort_compare_default_line(const char* left, const char* right) {
    return sort_cmp_sign(strcoll(left, right));
}

static int sort_compare_text_with_order(const sort_order_opts_t* order, const char* left, const char* right) {
    int cmp = 0;

    switch (order->mode) {
        case SORT_MODE_RANDOM:
            cmp = sort_cmp_sign(memcmp(left, right, BX_SHA256_DIGEST_SIZE));
            if (cmp == 0) {
                cmp = sort_cmp_sign(strcmp(left + BX_SHA256_DIGEST_SIZE, right + BX_SHA256_DIGEST_SIZE));
            }
            break;
        case SORT_MODE_NUMERIC:
            cmp = sort_compare_numeric_key(left, right);
            break;
        case SORT_MODE_HUMAN:
            cmp = sort_compare_human_key(left, right);
            break;
        case SORT_MODE_VERSION:
            cmp = sort_compare_version_key(left, right, order->ignore_case);
            break;
        case SORT_MODE_LEXICOGRAPHIC:
        default:
            if (order->ignore_case) {
                cmp = sort_cmp_sign(strcasecmp(left, right));
            }
            else {
                cmp = sort_cmp_sign(strcoll(left, right));
            }
            break;
    }

    if (order->reverse) {
        cmp = -cmp;
    }
    return cmp;
}

static sort_order_opts_t sort_primary_order_for_key(const sort_opts_t* opts, const sort_key_spec_t* spec, bool include_reverse) {
    sort_order_opts_t order = {
        .mode = spec->has_mode ? spec->mode : opts->mode,
        .ignore_case = (opts->ignore_case || spec->ignore_case),
        .reverse = include_reverse ? (opts->reverse || spec->reverse) : false,
    };
    return order;
}

static sort_order_opts_t sort_primary_order_for_line(const sort_opts_t* opts, bool include_reverse) {
    sort_order_opts_t order = {
        .mode = opts->mode,
        .ignore_case = opts->ignore_case,
        .reverse = include_reverse ? opts->reverse : false,
    };
    return order;
}

static int sort_compare_primary(const sort_opts_t* opts,
                                const char* left_line,
                                char* const* left_keys,
                                const char* right_line,
                                char* const* right_keys,
                                bool include_reverse) {
    if (opts->key_specs.len == 0) {
        const char* left_key = opts->ignore_leading_blanks ? sort_skip_leading_blanks(left_line) : left_line;
        const char* right_key = opts->ignore_leading_blanks ? sort_skip_leading_blanks(right_line) : right_line;
        sort_order_opts_t order = sort_primary_order_for_line(opts, include_reverse);
        if (order.mode == SORT_MODE_RANDOM) {
            return sort_compare_text_with_order(&order, left_keys[0], right_keys[0]);
        }
        return sort_compare_text_with_order(&order, left_key, right_key);
    }

    for (size_t i = 0; i < opts->key_specs.len; i++) {
        sort_order_opts_t order = sort_primary_order_for_key(opts, &opts->key_specs.items[i], include_reverse);
        int cmp = sort_compare_text_with_order(&order, left_keys[i], right_keys[i]);
        if (cmp != 0) {
            return cmp;
        }
    }

    return 0;
}

static int sort_compare_for_output(const sort_opts_t* opts,
                                   const char* left_line,
                                   char* const* left_keys,
                                   const char* right_line,
                                   char* const* right_keys) {
    int cmp = sort_compare_primary(opts, left_line, left_keys, right_line, right_keys, true);

    if (cmp == 0 && !opts->stable && !opts->unique) {
        cmp = sort_compare_default_line(left_line, right_line);
        if (opts->reverse) {
            cmp = -cmp;
        }
    }

    return cmp;
}

static int sort_compare_primary_for_unique(const sort_opts_t* opts,
                                           const char* left_line,
                                           char* const* left_keys,
                                           const char* right_line,
                                           char* const* right_keys) {
    return sort_compare_primary(opts, left_line, left_keys, right_line, right_keys, false);
}

static int sort_qsort_compare(const void* left_ptr, const void* right_ptr, void* arg) {
    const sort_opts_t* opts = (const sort_opts_t*)arg;
    const sort_line_t* left = (const sort_line_t*)left_ptr;
    const sort_line_t* right = (const sort_line_t*)right_ptr;

    int cmp = sort_compare_for_output(opts, left->text, left->key_texts, right->text, right->key_texts);
    if (cmp != 0) {
        return cmp;
    }

    if (left->original_index < right->original_index) {
        return -1;
    }
    if (left->original_index > right->original_index) {
        return 1;
    }
    return 0;
}

static sort_stream_status_t sort_read_stream(FILE* stream, const char* source_label, const sort_opts_t* opts, sort_line_vec_t* lines, size_t* next_index, const char* progname) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    int delimiter = opts->zero_terminated ? '\0' : '\n';

    while ((len = getdelim(&line, &line_cap, delimiter, stream)) != -1) {
        if (len > 0 && line[len - 1] == delimiter) {
            line[len - 1] = '\0';
        }

        if (*next_index == SIZE_MAX) {
            sort_report_memory_exhausted(progname);
            free(line);
            return SORT_STREAM_NOMEM;
        }

        char* copy = strdup(line);
        if (!copy) {
            sort_report_memory_exhausted(progname);
            free(line);
            return SORT_STREAM_NOMEM;
        }

        char** key_texts = NULL;
        if (sort_cached_key_count(opts) > 0) {
            key_texts = sort_make_key_copies(opts, copy);
            if (!key_texts) {
                sort_report_memory_exhausted(progname);
                free(copy);
                free(line);
                return SORT_STREAM_NOMEM;
            }
        }

        if (!sort_line_vec_push(lines, copy, key_texts, *next_index)) {
            sort_report_memory_exhausted(progname);
            free(copy);
            sort_free_key_texts(key_texts, sort_cached_key_count(opts));
            free(line);
            return SORT_STREAM_NOMEM;
        }
        (*next_index)++;
    }

    free(line);
    if (ferror(stream) || !feof(stream)) {
        int error = errno;
        sort_report_errno(progname, source_label);
        return error == ENOMEM ? SORT_STREAM_NOMEM : SORT_STREAM_ERROR;
    }
    return SORT_STREAM_OK;
}

static sort_stream_status_t
sort_check_stream(FILE* stream,
                  const char* source_label,
                  const sort_opts_t* opts,
                  char** previous_line,
                  char*** previous_keys,
                  bool* have_previous_line,
                  const char* progname) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t len;
    size_t line_number = 0;
    int delimiter = opts->zero_terminated ? '\0' : '\n';

    while ((len = getdelim(&line, &line_cap, delimiter, stream)) != -1) {
        if (len > 0 && line[len - 1] == delimiter) {
            line[len - 1] = '\0';
        }
        line_number++;

        char** line_keys = NULL;
        if (sort_cached_key_count(opts) > 0) {
            line_keys = sort_make_key_copies(opts, line);
            if (!line_keys) {
                sort_report_memory_exhausted(progname);
                free(line);
                return SORT_STREAM_NOMEM;
            }
        }

        if (*have_previous_line) {
            int cmp = sort_compare_for_output(opts, *previous_line, *previous_keys, line, line_keys);
            if (cmp > 0 || (opts->unique && cmp == 0)) {
                if (!opts->check_quiet) {
                    fprintf(stderr, "%s: %s:%zu: disorder\n", progname, source_label, line_number);
                }
                sort_free_key_texts(line_keys, sort_cached_key_count(opts));
                free(line);
                return SORT_STREAM_DISORDER;
            }
        }

        char* copy = strdup(line);
        if (!copy) {
            sort_report_memory_exhausted(progname);
            sort_free_key_texts(line_keys, sort_cached_key_count(opts));
            free(line);
            return SORT_STREAM_NOMEM;
        }

        free(*previous_line);
        *previous_line = copy;
        sort_free_key_texts(*previous_keys, sort_cached_key_count(opts));
        *previous_keys = line_keys;
        line_keys = NULL;
        *have_previous_line = true;
        sort_free_key_texts(line_keys, sort_cached_key_count(opts));
    }

    free(line);
    if (ferror(stream) || !feof(stream)) {
        sort_report_errno(progname, source_label);
        return SORT_STREAM_ERROR;
    }

    return SORT_STREAM_OK;
}

static bool sort_write_line(struct bx_line_writer* writer, const char* line, char delimiter) {
    return bx_line_writer_puts(writer, line) && bx_line_writer_putc(writer, delimiter);
}

int bx_sort_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'b', 'b', "ignore-leading-blanks", BX_OPTION_NO_VALUE},
        {'h', 'h', "human-numeric-sort", BX_OPTION_NO_VALUE},
        {'n', 'n', "numeric-sort", BX_OPTION_NO_VALUE},
        {'r', 'r', "reverse", BX_OPTION_NO_VALUE},
        {'u', 'u', "unique", BX_OPTION_NO_VALUE},
        {'f', 'f', "ignore-case", BX_OPTION_NO_VALUE},
        {'o', 'o', "output", BX_OPTION_REQUIRED_VALUE},
        {'c', 'c', "check", BX_OPTION_NO_VALUE},
        {'C', 'C', NULL, BX_OPTION_NO_VALUE},
        {'s', 's', "stable", BX_OPTION_NO_VALUE},
        {'t', 't', "field-separator", BX_OPTION_REQUIRED_VALUE},
        {'k', 'k', "key", BX_OPTION_REQUIRED_VALUE},
        {'z', 'z', "zero-terminated", BX_OPTION_NO_VALUE},
        {'V', 'V', "version-sort", BX_OPTION_NO_VALUE},
        {'R', 'R', "random-sort", BX_OPTION_NO_VALUE},
        {SORT_OPT_RANDOM_SOURCE, 0, "random-source", BX_OPTION_REQUIRED_VALUE},
        {SORT_OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {SORT_OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sort");
    struct bx_diag_ctx diag = {
        .progname = progname,
        .exit_status = 1,
    };
    (void)setlocale(LC_ALL, "");

    sort_opts_t opts = {.mode = SORT_MODE_LEXICOGRAPHIC};
    bool has_global_mode = false;
    bool had_error = true;
    const char** inputs = argc > 1 ? calloc((size_t)(argc - 1), sizeof(*inputs)) : NULL;
    if (argc > 1 && !inputs) {
        sort_report_memory_exhausted(progname);
        return 1;
    }
    size_t input_count = 0;
    bool empty_input = false;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc,
                          BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            empty_input |= option.token[0] == '\0';
            inputs[input_count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE && option.short_name)
                bx_diag(&diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option requires an argument -- '%s'", option.token);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'b':
                opts.ignore_leading_blanks = true;
                break;
            case 'h':
                if (!sort_set_mode(&opts.mode, &has_global_mode, SORT_MODE_HUMAN)) {
                    fprintf(stderr, "%s: incompatible sorting options\n", progname);
                    goto done;
                }
                break;
            case 'n':
                if (!sort_set_mode(&opts.mode, &has_global_mode, SORT_MODE_NUMERIC)) {
                    fprintf(stderr, "%s: incompatible sorting options\n", progname);
                    goto done;
                }
                break;
            case 'r':
                opts.reverse = true;
                break;
            case 'u':
                opts.unique = true;
                break;
            case 'f':
                opts.ignore_case = true;
                break;
            case 'o':
                opts.output_path = option.value;
                break;
            case 'c':
                opts.check = true;
                opts.check_quiet = false;
                break;
            case 'C':
                opts.check = true;
                opts.check_quiet = true;
                break;
            case 's':
                opts.stable = true;
                break;
            case 't':
                if (option.value[0] == '\0') {
                    fprintf(stderr, "%s: empty tab\n", progname);
                    goto done;
                }
                if (option.value[1] != '\0') {
                    fprintf(stderr, "%s: multi-character tab '%s'\n", progname, option.value);
                    goto done;
                }
                opts.has_field_separator = true;
                opts.field_separator = (unsigned char)option.value[0];
                break;
            case 'k': {
                sort_key_spec_t spec = {0};
                if (!sort_parse_key_spec(option.value, &spec)) {
                    fprintf(stderr, "%s: invalid key specification '%s'\n", progname, option.value);
                    goto done;
                }
                if (!sort_key_spec_vec_push(&opts.key_specs, &spec)) {
                    sort_report_memory_exhausted(progname);
                    goto done;
                }
                break;
            }
            case 'z':
                opts.zero_terminated = true;
                break;
            case 'V':
                if (!sort_set_mode(&opts.mode, &has_global_mode, SORT_MODE_VERSION)) {
                    fprintf(stderr, "%s: incompatible sorting options\n", progname);
                    goto done;
                }
                break;
            case 'R':
                if (!sort_set_mode(&opts.mode, &has_global_mode, SORT_MODE_RANDOM)) {
                    fprintf(stderr, "%s: incompatible sorting options\n", progname);
                    goto done;
                }
                break;
            case SORT_OPT_RANDOM_SOURCE:
                opts.random_source = option.value;
                break;
            case SORT_OPT_HELP:
                sort_print_help(progname);
                had_error = false;
                goto done;
            case SORT_OPT_VERSION:
                printf("sort (bx) %s\n", BX_VERSION);
                had_error = false;
                goto done;
        }
    }

    if (opts.check && opts.output_path) {
        fprintf(stderr, "%s: cannot use -o with -c or -C\n", progname);
        goto done;
    }
    if (empty_input || (opts.output_path && opts.output_path[0] == '\0')) {
        bx_diag(&diag, "empty %s path", empty_input ? "input" : "output");
        goto done;
    }
    if (!sort_seed_random_order(&opts, progname)) {
        goto done;
    }

    had_error = false;

    if (opts.check) {
        char* previous_line = NULL;
        char** previous_keys = NULL;
        bool have_previous_line = false;

        if (input_count == 0) {
            sort_stream_status_t status = sort_check_stream(stdin, "standard input", &opts, &previous_line, &previous_keys, &have_previous_line, progname);
            if (status != SORT_STREAM_OK) {
                had_error = true;
            }
        }
        else {
            for (size_t i = 0; i < input_count; i++) {
                FILE* stream = NULL;
                const char* source_label = NULL;

                if (strcmp(inputs[i], "-") == 0) {
                    stream = stdin;
                    source_label = "standard input";
                }
                else {
                    source_label = inputs[i];
                    stream = fopen(inputs[i], "r");
                    if (!stream) {
                        sort_report_errno(progname, source_label);
                        had_error = true;
                        continue;
                    }
                }

                sort_stream_status_t status = sort_check_stream(stream, source_label, &opts, &previous_line, &previous_keys, &have_previous_line, progname);
                if (status != SORT_STREAM_OK) {
                    had_error = true;
                }

                if (stream != stdin && fclose(stream) != 0) {
                    sort_report_errno(progname, source_label);
                    had_error = true;
                }

                if (status == SORT_STREAM_DISORDER || status == SORT_STREAM_ERROR || status == SORT_STREAM_NOMEM) {
                    break;
                }
            }
        }

        free(previous_line);
        sort_free_key_texts(previous_keys, sort_cached_key_count(&opts));
        goto done;
    }

    sort_line_vec_t lines = {0};
    size_t next_index = 0;
    bool fatal_read_error = false;

    if (input_count == 0) {
        sort_stream_status_t status = sort_read_stream(stdin, "standard input", &opts, &lines, &next_index, progname);
        if (status != SORT_STREAM_OK) {
            had_error = true;
            fatal_read_error = (status == SORT_STREAM_NOMEM);
        }
    }
    else {
        for (size_t i = 0; i < input_count; i++) {
            FILE* stream = NULL;
            const char* source_label = NULL;

            if (strcmp(inputs[i], "-") == 0) {
                stream = stdin;
                source_label = "standard input";
            }
            else {
                source_label = inputs[i];
                stream = fopen(inputs[i], "r");
                if (!stream) {
                    sort_report_errno(progname, source_label);
                    had_error = true;
                    continue;
                }
            }

            sort_stream_status_t status = sort_read_stream(stream, source_label, &opts, &lines, &next_index, progname);
            if (status != SORT_STREAM_OK) {
                had_error = true;
                if (status == SORT_STREAM_NOMEM) {
                    fatal_read_error = true;
                }
            }

            if (stream != stdin && fclose(stream) != 0) {
                sort_report_errno(progname, source_label);
                had_error = true;
            }

            if (status == SORT_STREAM_ERROR || status == SORT_STREAM_NOMEM) {
                break;
            }
        }
    }

    /* Partial results may go to stdout, but never replace a named file. */
    if (fatal_read_error || (had_error && opts.output_path))
        goto free_lines;

    if (lines.len > 1) {
        qsort_r(lines.items, lines.len, sizeof(lines.items[0]), sort_qsort_compare, &opts);
    }

    FILE* out = stdout;
    const char* output_label = "standard output";
    bool output_opened = false;
    if (opts.output_path) {
        out = fopen(opts.output_path, "w");
        if (!out) {
            sort_report_errno(progname, opts.output_path);
            had_error = true;
        }
        else {
            output_label = opts.output_path;
            output_opened = true;
        }
    }

    if (out) {
        char output_buffer[8192];
        struct bx_line_writer writer;
        int out_fd = fileno(out);
        char delimiter = opts.zero_terminated ? '\0' : '\n';
        bool have_last_unique = false;
        size_t last_unique_index = 0;

        if (out_fd < 0) {
            sort_report_errno(progname, output_label);
            had_error = true;
        }
        else {
            bx_line_writer_init(&writer, out_fd, output_buffer, sizeof(output_buffer));

            for (size_t i = 0; i < lines.len; i++) {
                if (opts.unique && have_last_unique) {
                    if (sort_compare_primary_for_unique(&opts,
                                                        lines.items[last_unique_index].text,
                                                        lines.items[last_unique_index].key_texts,
                                                        lines.items[i].text,
                                                        lines.items[i].key_texts) == 0) {
                        continue;
                    }
                }

                if (!sort_write_line(&writer, lines.items[i].text, delimiter)) {
                    sort_report_errno(progname, output_label);
                    had_error = true;
                    break;
                }

                have_last_unique = true;
                last_unique_index = i;
            }

            if (bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
                sort_report_errno(progname, output_label);
                had_error = true;
            }
        }
    }

    if (output_opened) {
        if (fclose(out) != 0) {
            sort_report_errno(progname, output_label);
            had_error = true;
        }
    }

free_lines:
    sort_line_vec_free(&lines, sort_cached_key_count(&opts));
done:
    free(inputs);
    sort_key_spec_vec_free(&opts.key_specs);
    return had_error ? 1 : 0;
}
