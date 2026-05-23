#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"

enum {
    BX_UNIQ_OPT_HELP = 256,
    BX_UNIQ_OPT_VERSION,
};

typedef struct {
    const char* progname;
    bool count;
    bool repeated;
    bool unique;
    bool ignore_case;
    size_t skip_fields;
    size_t skip_chars;
    size_t check_chars;
    bool check_chars_set;
    bool zero_terminated;
    bool show_help;
    bool show_version;
} uniq_opts_t;

struct uniq_record {
    char* data;
    size_t len;
    size_t cap;
};

static void bx_uniq_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [INPUT [OUTPUT]]\n", progname);
    fprintf(stream, "Filter adjacent matching lines from INPUT (or standard input),\n");
    fprintf(stream, "writing to OUTPUT (or standard output).\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no options, matching lines are merged to the first occurrence.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --count             prefix lines by the number of occurrences\n");
    fprintf(stream, "  -d, --repeated          only print duplicate lines, one for each group\n");
    fprintf(stream, "  -f, --skip-fields=N     avoid comparing the first N fields\n");
    fprintf(stream, "  -i, --ignore-case       ignore differences in case when comparing\n");
    fprintf(stream, "  -s, --skip-chars=N      avoid comparing the first N bytes\n");
    fprintf(stream, "  -u, --unique            only print unique lines\n");
    fprintf(stream, "  -w, --check-chars=N     compare no more than N bytes in lines\n");
    fprintf(stream, "  -z, --zero-terminated   line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

static bool bx_uniq_parse_nonnegative_count(const char* text, size_t* out) {
    const unsigned char* p = (const unsigned char*)text;
    size_t value = 0;
    bool saturated = false;

    if (p == NULL || *p == '\0') {
        return false;
    }
    if (*p == '+') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }

    while (*p != '\0') {
        if (*p < '0' || *p > '9') {
            return false;
        }
        if (!saturated) {
            unsigned int digit = (unsigned int)(*p - '0');
            if (value > (SIZE_MAX - digit) / 10u) {
                value = SIZE_MAX;
                saturated = true;
            }
            else {
                value = value * 10u + digit;
            }
        }
        p++;
    }

    *out = value;
    return true;
}

static bool bx_uniq_parse_count_option(const char* text, size_t* out, const char* what, struct bx_diag_ctx* diag) {
    if (!bx_uniq_parse_nonnegative_count(text, out)) {
        bx_diag(diag, "%s: invalid number of %s", text, what);
        return false;
    }

    return true;
}

static void bx_uniq_diag_option_requires_arg(struct bx_diag_ctx* diag, int missing_optopt, int missing_optind, int argc, char* const argv[]) {
    if (missing_optind > 0 && missing_optind <= argc && argv[missing_optind - 1] != NULL && strncmp(argv[missing_optind - 1], "--", 2) == 0) {
        bx_diag(diag, "option '%s' requires an argument", argv[missing_optind - 1]);
        return;
    }

    bx_cli_diag_option_requires_arg(diag, missing_optopt, missing_optind, argc, argv);
}

static bool bx_uniq_parse_options(int argc, char** argv, uniq_opts_t* opts, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {{"count", no_argument, NULL, 'c'},
                                                 {"repeated", no_argument, NULL, 'd'},
                                                 {"unique", no_argument, NULL, 'u'},
                                                 {"ignore-case", no_argument, NULL, 'i'},
                                                 {"skip-fields", required_argument, NULL, 'f'},
                                                 {"skip-chars", required_argument, NULL, 's'},
                                                 {"check-chars", required_argument, NULL, 'w'},
                                                 {"zero-terminated", no_argument, NULL, 'z'},
                                                 {"help", no_argument, NULL, BX_UNIQ_OPT_HELP},
                                                 {"version", no_argument, NULL, BX_UNIQ_OPT_VERSION},
                                                 {NULL, 0, NULL, 0}};

    memset(opts, 0, sizeof(*opts));
    opts->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "uniq");
    diag->progname = opts->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, ":cduif:s:w:z", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'c':
                opts->count = true;
                break;
            case 'd':
                opts->repeated = true;
                break;
            case 'u':
                opts->unique = true;
                break;
            case 'i':
                opts->ignore_case = true;
                break;
            case 'f':
                if (!bx_uniq_parse_count_option(optarg, &opts->skip_fields, "fields to skip", diag)) {
                    return false;
                }
                break;
            case 's':
                if (!bx_uniq_parse_count_option(optarg, &opts->skip_chars, "bytes to skip", diag)) {
                    return false;
                }
                break;
            case 'w':
                if (!bx_uniq_parse_count_option(optarg, &opts->check_chars, "bytes to compare", diag)) {
                    return false;
                }
                opts->check_chars_set = true;
                break;
            case 'z':
                opts->zero_terminated = true;
                break;
            case BX_UNIQ_OPT_HELP:
                opts->show_help = true;
                return true;
            case BX_UNIQ_OPT_VERSION:
                opts->show_version = true;
                return true;
            case ':':
                bx_uniq_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(opts->progname);
                return false;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(opts->progname);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static size_t bx_uniq_compare_len(const char* data, size_t len, int delimiter) {
    if (len > 0 && (unsigned char)data[len - 1] == (unsigned char)delimiter) {
        return len - 1;
    }
    return len;
}

static bool bx_uniq_is_blank(unsigned char ch) {
    return ch == ' ' || ch == '\t';
}

static size_t skip_to_compare(const char* data, size_t len, const uniq_opts_t* opts) {
    size_t pos = 0;

    for (size_t f = 0; f < opts->skip_fields && pos < len; f++) {
        while (pos < len && bx_uniq_is_blank((unsigned char)data[pos])) {
            pos++;
        }
        while (pos < len && !bx_uniq_is_blank((unsigned char)data[pos])) {
            pos++;
        }
    }

    if (opts->skip_chars > len - pos) {
        return len;
    }
    return pos + opts->skip_chars;
}

static int bx_uniq_byte_compare(const char* s1, const char* s2, size_t len, bool ignore_case) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c1 = (unsigned char)s1[i];
        unsigned char c2 = (unsigned char)s2[i];
        if (ignore_case) {
            c1 = (unsigned char)tolower(c1);
            c2 = (unsigned char)tolower(c2);
        }
        if (c1 != c2) {
            return c1 < c2 ? -1 : 1;
        }
    }
    return 0;
}

static int line_compare(const struct uniq_record* a, const struct uniq_record* b, const uniq_opts_t* opts) {
    size_t a_cmp_len = bx_uniq_compare_len(a->data, a->len, opts->zero_terminated ? '\0' : '\n');
    size_t b_cmp_len = bx_uniq_compare_len(b->data, b->len, opts->zero_terminated ? '\0' : '\n');
    size_t a_pos = skip_to_compare(a->data, a_cmp_len, opts);
    size_t b_pos = skip_to_compare(b->data, b_cmp_len, opts);
    size_t a_remaining = a_cmp_len - a_pos;
    size_t b_remaining = b_cmp_len - b_pos;
    size_t a_compare = a_remaining;
    size_t b_compare = b_remaining;
    size_t common = 0;
    int cmp = 0;

    if (opts->check_chars_set) {
        if (a_compare > opts->check_chars) {
            a_compare = opts->check_chars;
        }
        if (b_compare > opts->check_chars) {
            b_compare = opts->check_chars;
        }
    }

    common = a_compare < b_compare ? a_compare : b_compare;
    cmp = bx_uniq_byte_compare(a->data + a_pos, b->data + b_pos, common, opts->ignore_case);
    if (cmp != 0) {
        return cmp;
    }

    if (a_compare == b_compare) {
        return 0;
    }
    return a_compare < b_compare ? -1 : 1;
}

static void bx_uniq_record_copy(struct uniq_record* record, const char* data, size_t len) {
    if (len > record->cap) {
        record->data = xrealloc(record->data, len);
        record->cap = len;
    }
    memcpy(record->data, data, len);
    record->len = len;
}

static bool bx_uniq_emit_record(FILE* out, const struct uniq_record* record, unsigned long long count, const uniq_opts_t* opts, struct bx_diag_ctx* diag) {
    if (opts->count && fprintf(out, "%7llu ", count) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (record->len > 0 && fwrite(record->data, 1, record->len, out) != record->len) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_uniq_should_print(unsigned long long count, const uniq_opts_t* opts) {
    if (opts->repeated && count == 1) {
        return false;
    }
    if (opts->unique && count > 1) {
        return false;
    }
    return true;
}

static bool do_uniq(FILE* in, FILE* out, const uniq_opts_t* opts, struct bx_diag_ctx* diag) {
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t read_len = 0;
    unsigned long long count = 0;
    int delimiter = opts->zero_terminated ? '\0' : '\n';
    struct uniq_record line_record = {0};
    struct uniq_record prev_record = {0};
    bool have_prev = false;
    bool ok = true;

    while ((read_len = getdelim(&line, &line_cap, delimiter, in)) != -1) {
        bx_uniq_record_copy(&line_record, line, (size_t)read_len);
        if (!have_prev) {
            bx_uniq_record_copy(&prev_record, line_record.data, line_record.len);
            count = 1;
            have_prev = true;
            continue;
        }

        if (line_compare(&line_record, &prev_record, opts) == 0) {
            count++;
        }
        else {
            if (bx_uniq_should_print(count, opts) && !bx_uniq_emit_record(out, &prev_record, count, opts, diag)) {
                ok = false;
                break;
            }

            bx_uniq_record_copy(&prev_record, line_record.data, line_record.len);
            count = 1;
        }
    }

    if (ok && ferror(in)) {
        bx_diag(diag, "read error: %s", strerror(errno));
        ok = false;
    }

    if (ok && have_prev && bx_uniq_should_print(count, opts)) {
        if (!bx_uniq_emit_record(out, &prev_record, count, opts, diag)) {
            ok = false;
        }
    }

    free(line);
    free(line_record.data);
    free(prev_record.data);
    return ok;
}

int bx_uniq_main(int argc, char** argv) {
    uniq_opts_t opts;
    struct bx_diag_ctx diag = {
        .progname = "uniq",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;
    FILE* in = stdin;
    FILE* out = stdout;
    bool in_is_stdio = true;
    bool out_is_stdio = true;

    if (!bx_uniq_parse_options(argc, argv, &opts, &first_operand, &diag)) {
        return diag.exit_status == 0 ? 1 : diag.exit_status;
    }
    if (opts.show_help) {
        bx_uniq_print_help(stdout, opts.progname);
        return 0;
    }
    if (opts.show_version) {
        bx_cli_print_version(opts.progname);
        return 0;
    }

    if (argc - first_operand > 2) {
        bx_cli_diag_extra_operand(&diag, argv[first_operand + 2]);
        bx_cli_print_try_help(opts.progname);
        return diag.exit_status;
    }

    if (first_operand < argc) {
        in = bx_fopen_dash(argv[first_operand], "r", &in_is_stdio);
        if (!in) {
            bx_perror_path(&diag, argv[first_operand]);
            return diag.exit_status;
        }
        first_operand++;
    }
    if (first_operand < argc) {
        out = bx_fopen_dash(argv[first_operand], "w", &out_is_stdio);
        if (!out) {
            bx_perror_path(&diag, argv[first_operand]);
            bx_fclose_nonstdio(in, in_is_stdio);
            return diag.exit_status;
        }
    }

    if (!do_uniq(in, out, &opts, &diag)) {
        bx_fclose_nonstdio(in, in_is_stdio);
        bx_fclose_nonstdio(out, out_is_stdio);
        return diag.exit_status == 0 ? 1 : diag.exit_status;
    }

    if (fflush(out) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }
    bx_fclose_nonstdio(in, in_is_stdio);
    bx_fclose_nonstdio(out, out_is_stdio);

    return diag.exit_status;
}
