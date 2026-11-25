#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/fopen_dash.h"
#include "lib/args_common.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

struct bx_join_options {
    const char* progname;
    int join_field1;
    int join_field2;
    bool print_unpairable1;
    bool print_unpairable2;
    bool suppress_joined;
    const char* empty_fill;
    bool ignore_case;
    char separator;
    bool zero_terminated;
    const char* paths[2];
    const char* extra;
    size_t path_count;
    bool show_help;
    bool show_version;
};

static void bx_join_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE1 FILE2\n", progname);
    fprintf(stream, "For each pair of input lines with identical join fields, write a line to\n");
    fprintf(stream, "standard output.  The default join field is the first, delimited\n");
    fprintf(stream, "by whitespace.\n");
    fprintf(stream, "\n");
    fprintf(stream, "When FILE1 or FILE2 (not both) is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a FILENUM        also print unpairable lines from file FILENUM, where\n");
    fprintf(stream, "                      FILENUM is 1 or 2, corresponding to FILE1 or FILE2\n");
    fprintf(stream, "  -e EMPTY          replace missing input fields with EMPTY\n");
    fprintf(stream, "  -i, --ignore-case  ignore differences in case when comparing fields\n");
    fprintf(stream, "  -j FIELD          equivalent to '-1 FIELD -2 FIELD'\n");
    fprintf(stream, "  -t CHAR           use CHAR as input and output field separator\n");
    fprintf(stream, "  -v FILENUM        like -a FILENUM, but suppress joined output lines\n");
    fprintf(stream, "  -1 FIELD          join on this FIELD of file 1\n");
    fprintf(stream, "  -2 FIELD          join on this FIELD of file 2\n");
    fprintf(stream, "  -z, --zero-terminated  line delimiter is NUL, not newline\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_join_parse_options(int argc, char** argv, struct bx_join_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'a', 'a', NULL, BX_OPTION_REQUIRED_VALUE},        {'e', 'e', NULL, BX_OPTION_REQUIRED_VALUE}, {'i', 'i', "ignore-case", BX_OPTION_NO_VALUE}, {'j', 'j', NULL, BX_OPTION_REQUIRED_VALUE},
        {'t', 't', NULL, BX_OPTION_REQUIRED_VALUE},        {'v', 'v', NULL, BX_OPTION_REQUIRED_VALUE}, {'1', '1', NULL, BX_OPTION_REQUIRED_VALUE},    {'2', '2', NULL, BX_OPTION_REQUIRED_VALUE},
        {'z', 'z', "zero-terminated", BX_OPTION_NO_VALUE}, {4, 0, "help", BX_OPTION_NO_VALUE},         {5, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "join";
    options->join_field1 = 1;
    options->join_field2 = 1;
    options->separator = 0;  // whitespace
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (options->path_count < 2)
                options->paths[options->path_count++] = option.token;
            else if (!options->extra)
                options->extra = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option requires an argument -- '%c'", option.short_name);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        switch (option.spec->id) {
            case 'a':
                if (strcmp(option.value, "1") == 0)
                    options->print_unpairable1 = true;
                else if (strcmp(option.value, "2") == 0)
                    options->print_unpairable2 = true;
                else {
                    bx_diag(diag, "invalid field number: '%s'", option.value);
                    return false;
                }
                break;
            case 'e':
                options->empty_fill = option.value;
                break;
            case 'i':
                options->ignore_case = true;
                break;
            case 'j':
                if (!bx_args_parse_int_range(option.value, 1, INT_MAX, &options->join_field1)) {
                    bx_diag(diag, "invalid field number: '%s'", option.value);
                    return false;
                }
                options->join_field2 = options->join_field1;
                break;
            case 't':
                if (strlen(option.value) != 1) {
                    bx_diag(diag, "the delimiter must be a single character");
                    return false;
                }
                options->separator = option.value[0];
                break;
            case 'v':
                options->suppress_joined = true;
                if (strcmp(option.value, "1") == 0)
                    options->print_unpairable1 = true;
                else if (strcmp(option.value, "2") == 0)
                    options->print_unpairable2 = true;
                else {
                    bx_diag(diag, "invalid field number: '%s'", option.value);
                    return false;
                }
                break;
            case '1':
                if (!bx_args_parse_int_range(option.value, 1, INT_MAX, &options->join_field1)) {
                    bx_diag(diag, "invalid field number: '%s'", option.value);
                    return false;
                }
                break;
            case '2':
                if (!bx_args_parse_int_range(option.value, 1, INT_MAX, &options->join_field2)) {
                    bx_diag(diag, "invalid field number: '%s'", option.value);
                    return false;
                }
                break;
            case 'z':
                options->zero_terminated = true;
                break;
            case 4:
                options->show_help = true;
                return true;
            case 5:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    return true;
}

struct line {
    char* data;
    char** fields;
    size_t nfields;
};

static void free_line(struct line* l) {
    free(l->data);
    free(l->fields);
}

static bool parse_line(struct line* l, char* data, char sep) {
    l->data = xstrdup(data);
    l->nfields = 0;
    size_t cap = 8;
    l->fields = xmalloc(cap * sizeof(char*));

    char* p = l->data;
    if (sep == 0) {
        // Default: whitespace delimited, leading/trailing whitespace ignored
        while (*p && isspace(*p))
            p++;
        while (*p) {
            if (l->nfields >= cap) {
                cap *= 2;
                l->fields = xrealloc(l->fields, cap * sizeof(char*));
            }
            l->fields[l->nfields++] = p;
            while (*p && !isspace(*p))
                p++;
            if (*p) {
                *p++ = '\0';
                while (*p && isspace(*p))
                    p++;
            }
        }
    }
    else {
        // Specified separator
        while (true) {
            if (l->nfields >= cap) {
                cap *= 2;
                l->fields = xrealloc(l->fields, cap * sizeof(char*));
            }
            l->fields[l->nfields++] = p;
            char* next = strchr(p, sep);
            if (next) {
                *next = '\0';
                p = next + 1;
            }
            else {
                break;
            }
        }
    }
    return true;
}

static const char* get_field(struct line* l, int idx, const char* empty) {
    if (idx <= 0 || (size_t)idx > l->nfields)
        return empty ? empty : "";
    return l->fields[idx - 1];
}

static int compare_lines(struct line* l1, int f1, struct line* l2, int f2, bool ignore_case) {
    const char* s1 = get_field(l1, f1, NULL);
    const char* s2 = get_field(l2, f2, NULL);
    if (ignore_case)
        return strcasecmp(s1, s2);
    return strcmp(s1, s2);
}

static bool bx_join_write_error(struct bx_diag_ctx* diag) {
    bx_diag(diag, "write error: %s", strerror(errno));
    return false;
}

static bool bx_join_write_text(
    struct bx_line_writer* writer,
    const char* text,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_puts(writer, text)) {
        return bx_join_write_error(diag);
    }
    return true;
}

static bool bx_join_write_char(
    struct bx_line_writer* writer,
    char ch,
    struct bx_diag_ctx* diag
) {
    if (!bx_line_writer_putc(writer, ch)) {
        return bx_join_write_error(diag);
    }
    return true;
}

static bool bx_join_write_record_delimiter(
    struct bx_line_writer* writer,
    const struct bx_join_options* options,
    struct bx_diag_ctx* diag
) {
    return bx_join_write_char(writer, options->zero_terminated ? '\0' : '\n', diag);
}

static bool print_joined(
    struct line* l1,
    struct line* l2,
    struct bx_join_options* options,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
    if (options->suppress_joined)
        return true;

    char sep = options->separator ? options->separator : ' ';

    // Default format: join field, rest of l1, rest of l2
    if (!bx_join_write_text(writer, get_field(l1, options->join_field1, options->empty_fill), diag)) {
        return false;
    }

    for (size_t i = 1; i <= l1->nfields; i++) {
        if ((int)i == options->join_field1)
            continue;
        if (!bx_join_write_char(writer, sep, diag) ||
            !bx_join_write_text(writer, get_field(l1, i, options->empty_fill), diag)) {
            return false;
        }
    }
    for (size_t i = 1; i <= l2->nfields; i++) {
        if ((int)i == options->join_field2)
            continue;
        if (!bx_join_write_char(writer, sep, diag) ||
            !bx_join_write_text(writer, get_field(l2, i, options->empty_fill), diag)) {
            return false;
        }
    }
    return bx_join_write_record_delimiter(writer, options, diag);
}

static bool print_unpairable(
    struct line* l,
    struct bx_join_options* options,
    struct bx_line_writer* writer,
    struct bx_diag_ctx* diag
) {
    char sep = options->separator ? options->separator : ' ';
    for (size_t i = 1; i <= l->nfields; i++) {
        if (i > 1 && !bx_join_write_char(writer, sep, diag)) {
            return false;
        }
        if (!bx_join_write_text(writer, get_field(l, i, options->empty_fill), diag)) {
            return false;
        }
    }
    return bx_join_write_record_delimiter(writer, options, diag);
}

int bx_join_main(int argc, char** argv) {
    struct bx_join_options options;
    struct bx_diag_ctx diag = {.progname = "join", .exit_status = 0};

    if (!bx_join_parse_options(argc, argv, &options, &diag))
        return 1;
    if (options.show_help) {
        bx_join_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (options.path_count != 2) {
        bx_diag(&diag, "missing operand");
        return 1;
    }

    if (options.extra) {
        bx_cli_diag_extra_operand(&diag, options.extra);
        return 1;
    }
    if (!*options.paths[0] || !*options.paths[1]) {
        bx_diag(&diag, "empty file operand");
        return 1;
    }
    if (strcmp(options.paths[0], "-") == 0 && strcmp(options.paths[1], "-") == 0) {
        bx_diag(&diag, "only one input may be standard input");
        return 1;
    }

    bool f1_is_stdio = false;
    FILE* f1 = bx_fopen_dash(options.paths[0], "r", &f1_is_stdio);
    if (!f1) {
        bx_diag(&diag, "%s: %s", options.paths[0], strerror(errno));
        return 1;
    }
    bool f2_is_stdio = false;
    FILE* f2 = bx_fopen_dash(options.paths[1], "r", &f2_is_stdio);
    if (!f2) {
        bx_diag(&diag, "%s: %s", options.paths[1], strerror(errno));
        bx_fclose_nonstdio(f1, f1_is_stdio);
        return 1;
    }

    char *data1 = NULL, *data2 = NULL;
    size_t cap1 = 0, cap2 = 0;
    ssize_t len1, len2;
    int delimiter = options.zero_terminated ? '\0' : '\n';

    struct line l1 = {0}, l2 = {0};
    bool has_l1 = false, has_l2 = false;
    bool ok = true;
    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    len1 = getdelim(&data1, &cap1, delimiter, f1);
    if (len1 != -1) {
        if (data1[len1 - 1] == delimiter)
            data1[len1 - 1] = '\0';
        parse_line(&l1, data1, options.separator);
        has_l1 = true;
    }

    len2 = getdelim(&data2, &cap2, delimiter, f2);
    if (len2 != -1) {
        if (data2[len2 - 1] == delimiter)
            data2[len2 - 1] = '\0';
        parse_line(&l2, data2, options.separator);
        has_l2 = true;
    }

    while (ok && (has_l1 || has_l2)) {
        int cmp;
        if (has_l1 && has_l2) {
            cmp = compare_lines(&l1, options.join_field1, &l2, options.join_field2, options.ignore_case);
            if (cmp == 0) {
                ok = print_joined(&l1, &l2, &options, &writer, &diag);
                // Simple implementation doesn't handle duplicate keys yet
                free_line(&l1);
                has_l1 = false;
                free_line(&l2);
                has_l2 = false;
                if (!ok)
                    continue;
                len1 = getdelim(&data1, &cap1, delimiter, f1);
                if (len1 != -1) {
                    if (data1[len1 - 1] == delimiter)
                        data1[len1 - 1] = '\0';
                    parse_line(&l1, data1, options.separator);
                    has_l1 = true;
                }
                len2 = getdelim(&data2, &cap2, delimiter, f2);
                if (len2 != -1) {
                    if (data2[len2 - 1] == delimiter)
                        data2[len2 - 1] = '\0';
                    parse_line(&l2, data2, options.separator);
                    has_l2 = true;
                }
            }
            else if (cmp < 0) {
                if (options.print_unpairable1)
                    ok = print_unpairable(&l1, &options, &writer, &diag);
                free_line(&l1);
                has_l1 = false;
                if (!ok)
                    continue;
                len1 = getdelim(&data1, &cap1, delimiter, f1);
                if (len1 != -1) {
                    if (data1[len1 - 1] == delimiter)
                        data1[len1 - 1] = '\0';
                    parse_line(&l1, data1, options.separator);
                    has_l1 = true;
                }
            }
            else {
                if (options.print_unpairable2)
                    ok = print_unpairable(&l2, &options, &writer, &diag);
                free_line(&l2);
                has_l2 = false;
                if (!ok)
                    continue;
                len2 = getdelim(&data2, &cap2, delimiter, f2);
                if (len2 != -1) {
                    if (data2[len2 - 1] == delimiter)
                        data2[len2 - 1] = '\0';
                    parse_line(&l2, data2, options.separator);
                    has_l2 = true;
                }
            }
        }
        else if (has_l1) {
            if (options.print_unpairable1)
                ok = print_unpairable(&l1, &options, &writer, &diag);
            free_line(&l1);
            has_l1 = false;
            if (!ok)
                continue;
            len1 = getdelim(&data1, &cap1, delimiter, f1);
            if (len1 != -1) {
                if (data1[len1 - 1] == delimiter)
                    data1[len1 - 1] = '\0';
                parse_line(&l1, data1, options.separator);
                has_l1 = true;
            }
        }
        else {
            if (options.print_unpairable2)
                ok = print_unpairable(&l2, &options, &writer, &diag);
            free_line(&l2);
            has_l2 = false;
            if (!ok)
                continue;
            len2 = getdelim(&data2, &cap2, delimiter, f2);
            if (len2 != -1) {
                if (data2[len2 - 1] == delimiter)
                    data2[len2 - 1] = '\0';
                parse_line(&l2, data2, options.separator);
                has_l2 = true;
            }
        }
    }

    if (has_l1) {
        free_line(&l1);
    }
    if (has_l2) {
        free_line(&l2);
    }

    if (ok && bx_line_writer_error(&writer) == 0 && !bx_line_writer_flush(&writer)) {
        bx_join_write_error(&diag);
    }

    free(data1);
    free(data2);
    bx_fclose_nonstdio(f1, f1_is_stdio);
    bx_fclose_nonstdio(f2, f2_is_stdio);
    return diag.exit_status;
}
