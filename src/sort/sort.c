#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "diag.h"

typedef enum {
    SORT_MODE_LEXICOGRAPHIC,
    SORT_MODE_NUMERIC,
    SORT_MODE_VERSION,
} sort_mode_t;

typedef struct {
    size_t start_field;
    size_t end_field;
} sort_key_spec_t;

typedef struct {
    sort_mode_t mode;
    bool reverse;
    bool unique;
    bool ignore_case;
    bool check;
    bool check_quiet;
    bool stable;
    bool zero_terminated;
    bool has_field_separator;
    unsigned char field_separator;
    bool has_key_spec;
    sort_key_spec_t key_spec;
    const char* output_path;
} sort_opts_t;

typedef struct {
    char* text;
    char* key_text;
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
    bool has_digits;
    bool is_zero;
} numeric_token_t;

typedef enum {
    SORT_STREAM_OK,
    SORT_STREAM_ERROR,
    SORT_STREAM_NOMEM,
    SORT_STREAM_DISORDER,
} sort_stream_status_t;

enum {
    SORT_OPT_HELP = 256,
    SORT_OPT_VERSION,
};

static int sort_cmp_sign(int value) {
    if (value < 0)
        return -1;
    if (value > 0)
        return 1;
    return 0;
}

static void sort_report_errno(const char* progname, const char* path) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errno));
}

static void sort_report_memory_exhausted(const char* progname) {
    fprintf(stderr, "%s: memory exhausted\n", progname);
}

static void sort_print_help(const char* progname) {
    printf("Usage: %s [OPTION]... [FILE]...\n", progname);
    puts("Write sorted concatenation of all FILE(s) to standard output.");
    puts("");
    puts("Ordering options:");
    puts("  -n, --numeric-sort      compare according to numeric value");
    puts("  -V, --version-sort      natural sort of version numbers within text");
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

static bool sort_line_vec_push(sort_line_vec_t* vec, char* text, char* key_text, size_t original_index) {
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
    vec->items[vec->len].key_text = key_text;
    vec->items[vec->len].original_index = original_index;
    vec->len++;
    return true;
}

static void sort_line_vec_free(sort_line_vec_t* vec) {
    if (!vec->items) {
        return;
    }
    for (size_t i = 0; i < vec->len; i++) {
        free(vec->items[i].text);
        free(vec->items[i].key_text);
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

static bool sort_parse_key_spec(const char* text, sort_key_spec_t* spec_out) {
    const char* p = text;
    sort_key_spec_t spec = {0};

    if (!sort_parse_positive_index(&p, &spec.start_field)) {
        return false;
    }

    if (*p == '\0') {
        spec.end_field = 0;
    }
    else if (*p == ',') {
        p++;
        if (!sort_parse_positive_index(&p, &spec.end_field)) {
            return false;
        }
        if (*p != '\0') {
            return false;
        }
    }
    else {
        return false;
    }

    *spec_out = spec;
    return true;
}

static bool sort_find_field_range(const char* line, const sort_opts_t* opts, size_t field_number, const char** field_start, const char** field_end) {
    const char* line_end = line + strlen(line);

    if (opts->has_field_separator) {
        const char* cursor = line;
        size_t current = 1;
        while (current < field_number) {
            const char* separator = strchr(cursor, (int)opts->field_separator);
            if (!separator) {
                *field_start = line_end;
                *field_end = line_end;
                return false;
            }
            cursor = separator + 1;
            current++;
        }

        const char* separator = strchr(cursor, (int)opts->field_separator);
        *field_start = cursor;
        *field_end = separator ? separator : line_end;
        return true;
    }

    const unsigned char* cursor = (const unsigned char*)line;
    while (*cursor != '\0' && isblank(*cursor)) {
        cursor++;
    }
    if (*cursor == '\0') {
        *field_start = line_end;
        *field_end = line_end;
        return false;
    }

    size_t current = 1;
    while (current < field_number) {
        while (*cursor != '\0' && !isblank(*cursor)) {
            cursor++;
        }
        while (*cursor != '\0' && isblank(*cursor)) {
            cursor++;
        }
        if (*cursor == '\0') {
            *field_start = line_end;
            *field_end = line_end;
            return false;
        }
        current++;
    }

    const unsigned char* start = cursor;
    while (*cursor != '\0' && !isblank(*cursor)) {
        cursor++;
    }

    *field_start = (const char*)start;
    *field_end = (const char*)cursor;
    return true;
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

static char* sort_make_key_copy(const sort_opts_t* opts, const char* line) {
    const char* line_end = line + strlen(line);
    const char* key_start = line;
    const char* key_end = line_end;

    if (opts->has_key_spec) {
        const char* start_field_begin = NULL;
        const char* ignored_end = NULL;
        bool has_start_field = sort_find_field_range(line, opts, opts->key_spec.start_field, &start_field_begin, &ignored_end);
        if (!has_start_field) {
            key_start = line_end;
            key_end = line_end;
        }
        else {
            key_start = start_field_begin;
            if (opts->key_spec.end_field == 0) {
                key_end = line_end;
            }
            else {
                const char* end_field_end = NULL;
                bool has_end_field = sort_find_field_range(line, opts, opts->key_spec.end_field, &ignored_end, &end_field_end);
                key_end = has_end_field ? end_field_end : line_end;
            }
            if (key_end < key_start) {
                key_end = key_start;
            }
        }
    }

    return sort_strdup_range(key_start, (size_t)(key_end - key_start));
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

static int sort_compare_key_text(const sort_opts_t* opts, const char* left, const char* right) {
    switch (opts->mode) {
        case SORT_MODE_NUMERIC:
            return sort_compare_numeric_key(left, right);
        case SORT_MODE_VERSION:
            return sort_compare_version_key(left, right, opts->ignore_case);
        case SORT_MODE_LEXICOGRAPHIC:
        default:
            if (opts->ignore_case) {
                return sort_cmp_sign(strcasecmp(left, right));
            }
            return sort_cmp_sign(strcoll(left, right));
    }
}

static int sort_compare_for_output(const sort_opts_t* opts, const char* left_line, const char* left_key, const char* right_line, const char* right_key) {
    int cmp = sort_compare_key_text(opts, left_key, right_key);

    if (cmp == 0 && !opts->stable && !opts->unique) {
        cmp = sort_compare_default_line(left_line, right_line);
    }

    if (opts->reverse) {
        cmp = -cmp;
    }

    return cmp;
}

static const char* sort_line_key_text(const sort_opts_t* opts, const sort_line_t* line) {
    if (opts->has_key_spec && line->key_text) {
        return line->key_text;
    }
    return line->text;
}

static int sort_qsort_compare(const void* left_ptr, const void* right_ptr, void* arg) {
    const sort_opts_t* opts = (const sort_opts_t*)arg;
    const sort_line_t* left = (const sort_line_t*)left_ptr;
    const sort_line_t* right = (const sort_line_t*)right_ptr;
    const char* left_key = sort_line_key_text(opts, left);
    const char* right_key = sort_line_key_text(opts, right);

    int cmp = sort_compare_for_output(opts, left->text, left_key, right->text, right_key);
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

        char* key_copy = NULL;
        if (opts->has_key_spec) {
            key_copy = sort_make_key_copy(opts, copy);
            if (!key_copy) {
                sort_report_memory_exhausted(progname);
                free(copy);
                free(line);
                return SORT_STREAM_NOMEM;
            }
        }

        if (!sort_line_vec_push(lines, copy, key_copy, *next_index)) {
            sort_report_memory_exhausted(progname);
            free(copy);
            free(key_copy);
            free(line);
            return SORT_STREAM_NOMEM;
        }
        (*next_index)++;
    }

    free(line);
    if (ferror(stream)) {
        sort_report_errno(progname, source_label);
        return SORT_STREAM_ERROR;
    }
    return SORT_STREAM_OK;
}

static sort_stream_status_t
sort_check_stream(FILE* stream, const char* source_label, const sort_opts_t* opts, char** previous_line, char** previous_key, bool* have_previous_line, const char* progname) {
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

        char* line_key = NULL;
        if (opts->has_key_spec) {
            line_key = sort_make_key_copy(opts, line);
            if (!line_key) {
                sort_report_memory_exhausted(progname);
                free(line);
                return SORT_STREAM_NOMEM;
            }
        }

        if (*have_previous_line) {
            const char* left_key = opts->has_key_spec ? *previous_key : *previous_line;
            const char* right_key = opts->has_key_spec ? line_key : line;
            int cmp = sort_compare_for_output(opts, *previous_line, left_key, line, right_key);
            if (cmp > 0 || (opts->unique && cmp == 0)) {
                if (!opts->check_quiet) {
                    fprintf(stderr, "%s: %s:%zu: disorder\n", progname, source_label, line_number);
                }
                free(line_key);
                free(line);
                return SORT_STREAM_DISORDER;
            }
        }

        char* copy = strdup(line);
        if (!copy) {
            sort_report_memory_exhausted(progname);
            free(line_key);
            free(line);
            return SORT_STREAM_NOMEM;
        }

        free(*previous_line);
        *previous_line = copy;
        if (opts->has_key_spec) {
            free(*previous_key);
            *previous_key = line_key;
            line_key = NULL;
        }
        *have_previous_line = true;
        free(line_key);
    }

    free(line);
    if (ferror(stream)) {
        sort_report_errno(progname, source_label);
        return SORT_STREAM_ERROR;
    }

    return SORT_STREAM_OK;
}

static bool sort_write_line(FILE* out, const char* line, int delimiter) {
    if (fputs(line, out) == EOF) {
        return false;
    }
    if (fputc(delimiter, out) == EOF) {
        return false;
    }
    return true;
}

int bx_sort_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"numeric-sort", no_argument, NULL, 'n'},
        {"reverse", no_argument, NULL, 'r'},
        {"unique", no_argument, NULL, 'u'},
        {"ignore-case", no_argument, NULL, 'f'},
        {"output", required_argument, NULL, 'o'},
        {"check", no_argument, NULL, 'c'},
        {"stable", no_argument, NULL, 's'},
        {"field-separator", required_argument, NULL, 't'},
        {"key", required_argument, NULL, 'k'},
        {"zero-terminated", no_argument, NULL, 'z'},
        {"version-sort", no_argument, NULL, 'V'},
        {"help", no_argument, NULL, SORT_OPT_HELP},
        {"version", no_argument, NULL, SORT_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    const char* progname = (argv[0] && argv[0][0]) ? argv[0] : "sort";
    (void)setlocale(LC_ALL, "");

    sort_opts_t opts = {
        .mode = SORT_MODE_LEXICOGRAPHIC,
        .reverse = false,
        .unique = false,
        .ignore_case = false,
        .check = false,
        .check_quiet = false,
        .stable = false,
        .zero_terminated = false,
        .has_field_separator = false,
        .field_separator = 0,
        .has_key_spec = false,
        .key_spec = {.start_field = 1u, .end_field = 0u},
        .output_path = NULL,
    };

    int c;
    while ((c = getopt_long(argc, argv, "nrufo:cCszt:k:V", long_options, NULL)) != -1) {
        switch (c) {
            case 'n':
                opts.mode = SORT_MODE_NUMERIC;
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
                opts.output_path = optarg;
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
                if (!optarg || optarg[0] == '\0') {
                    fprintf(stderr, "%s: empty tab\n", progname);
                    return 1;
                }
                if (optarg[1] != '\0') {
                    fprintf(stderr, "%s: multi-character tab '%s'\n", progname, optarg);
                    return 1;
                }
                opts.has_field_separator = true;
                opts.field_separator = (unsigned char)optarg[0];
                break;
            case 'k':
                if (!sort_parse_key_spec(optarg, &opts.key_spec)) {
                    fprintf(stderr, "%s: invalid key specification '%s'\n", progname, optarg);
                    return 1;
                }
                opts.has_key_spec = true;
                break;
            case 'z':
                opts.zero_terminated = true;
                break;
            case 'V':
                opts.mode = SORT_MODE_VERSION;
                break;
            case SORT_OPT_HELP:
                sort_print_help(progname);
                return 0;
            case SORT_OPT_VERSION:
                printf("sort (bx) %s\n", BX_VERSION);
                return 0;
            default:
                return 1;
        }
    }

    if (opts.check && opts.output_path) {
        fprintf(stderr, "%s: cannot use -o with -c or -C\n", progname);
        return 1;
    }

    bool had_error = false;

    if (opts.check) {
        char* previous_line = NULL;
        char* previous_key = NULL;
        bool have_previous_line = false;

        if (optind == argc) {
            sort_stream_status_t status = sort_check_stream(stdin, "standard input", &opts, &previous_line, &previous_key, &have_previous_line, progname);
            if (status != SORT_STREAM_OK) {
                had_error = true;
            }
        }
        else {
            for (int i = optind; i < argc; i++) {
                FILE* stream = NULL;
                const char* source_label = NULL;

                if (strcmp(argv[i], "-") == 0) {
                    stream = stdin;
                    source_label = "standard input";
                }
                else {
                    source_label = argv[i];
                    stream = fopen(argv[i], "r");
                    if (!stream) {
                        sort_report_errno(progname, argv[i]);
                        had_error = true;
                        continue;
                    }
                }

                sort_stream_status_t status = sort_check_stream(stream, source_label, &opts, &previous_line, &previous_key, &have_previous_line, progname);
                if (status != SORT_STREAM_OK) {
                    had_error = true;
                }

                if (stream != stdin && fclose(stream) != 0) {
                    sort_report_errno(progname, argv[i]);
                    had_error = true;
                }

                if (status == SORT_STREAM_DISORDER || status == SORT_STREAM_ERROR || status == SORT_STREAM_NOMEM) {
                    break;
                }
            }
        }

        free(previous_line);
        free(previous_key);
        return had_error ? 1 : 0;
    }

    sort_line_vec_t lines = {0};
    size_t next_index = 0;
    bool fatal_read_error = false;

    if (optind == argc) {
        sort_stream_status_t status = sort_read_stream(stdin, "standard input", &opts, &lines, &next_index, progname);
        if (status != SORT_STREAM_OK) {
            had_error = true;
            fatal_read_error = (status == SORT_STREAM_NOMEM);
        }
    }
    else {
        for (int i = optind; i < argc; i++) {
            FILE* stream = NULL;
            const char* source_label = NULL;

            if (strcmp(argv[i], "-") == 0) {
                stream = stdin;
                source_label = "standard input";
            }
            else {
                stream = fopen(argv[i], "r");
                source_label = argv[i];
                if (!stream) {
                    sort_report_errno(progname, argv[i]);
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
                sort_report_errno(progname, argv[i]);
                had_error = true;
            }

            if (status == SORT_STREAM_ERROR || status == SORT_STREAM_NOMEM) {
                break;
            }
        }
    }

    if (!fatal_read_error && lines.len > 1) {
        qsort_r(lines.items, lines.len, sizeof(lines.items[0]), sort_qsort_compare, &opts);
    }

    FILE* out = stdout;
    const char* output_label = "standard output";
    bool output_opened = false;
    if (!fatal_read_error && opts.output_path) {
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

    if (!fatal_read_error && out) {
        int delimiter = opts.zero_terminated ? '\0' : '\n';
        bool have_last_unique = false;
        size_t last_unique_index = 0;

        for (size_t i = 0; i < lines.len; i++) {
            const char* current_key = sort_line_key_text(&opts, &lines.items[i]);
            if (opts.unique && have_last_unique) {
                const char* previous_key = sort_line_key_text(&opts, &lines.items[last_unique_index]);
                if (sort_compare_key_text(&opts, previous_key, current_key) == 0) {
                    continue;
                }
            }

            if (!sort_write_line(out, lines.items[i].text, delimiter)) {
                sort_report_errno(progname, output_label);
                had_error = true;
                break;
            }

            have_last_unique = true;
            last_unique_index = i;
        }
    }

    if (output_opened) {
        if (fclose(out) != 0) {
            sort_report_errno(progname, output_label);
            had_error = true;
        }
    }
    else if (!output_opened && out == stdout) {
        if (fflush(stdout) == EOF) {
            sort_report_errno(progname, output_label);
            had_error = true;
        }
    }

    sort_line_vec_free(&lines);
    return had_error ? 1 : 0;
}
