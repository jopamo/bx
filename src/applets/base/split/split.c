#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"

static void next_suffix(char* suffix, int length, bool numeric, bool hex) {
    for (int i = length - 1; i >= 0; i--) {
        if (numeric) {
            if (suffix[i] < '9') {
                suffix[i]++;
                return;
            }
            suffix[i] = '0';
        }
        else if (hex) {
            if (suffix[i] < '9') {
                suffix[i]++;
                return;
            }
            if (suffix[i] == '9') {
                suffix[i] = 'a';
                return;
            }
            if (suffix[i] < 'f') {
                suffix[i]++;
                return;
            }
            suffix[i] = '0';
        }
        else {
            if (suffix[i] < 'z') {
                suffix[i]++;
                return;
            }
            suffix[i] = 'a';
        }
    }
}

static void bx_split_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE [PREFIX]]\n", progname);
    fprintf(stream, "Output pieces of FILE to PREFIXaa, PREFIXab, ...; default size is 1000 lines.\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no FILE, or when FILE is -, read standard input.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --suffix-length=N      generate suffixes of length N (default 2)\n");
    fprintf(stream, "  -b, --bytes=SIZE           put SIZE bytes per output file\n");
    fprintf(stream, "  -d, --numeric-suffixes     use numeric suffixes instead of alphabetic\n");
    fprintf(stream, "  -l, --lines=NUMBER         put NUMBER lines per output file\n");
    fprintf(stream, "  -x, --hex-suffixes         use hexadecimal suffixes instead of alphabetic\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

int bx_split_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'b', 'b', "bytes", BX_OPTION_REQUIRED_VALUE},
        {'l', 'l', "lines", BX_OPTION_REQUIRED_VALUE},
        {'a', 'a', "suffix-length", BX_OPTION_REQUIRED_VALUE},
        {'d', 'd', "numeric-suffixes", BX_OPTION_NO_VALUE},
        {'x', 'x', "hex-suffixes", BX_OPTION_NO_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'v', 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "split");
    struct bx_diag_ctx diag = {.progname = progname};
    long long split_size = 0;
    bool split_by_lines = true;
    long long lines_per_file = 1000;
    int suffix_length = 2;
    bool numeric = false;
    bool hex = false;

    const char* operands[2];
    size_t count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (count == sizeof(operands) / sizeof(operands[0])) {
                bx_cli_diag_extra_operand(&diag, option.token);
                return 1;
            }
            operands[count++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option '%s' requires an argument", option.token);
            else if (option.event == BX_OPTION_UNEXPECTED_VALUE)
                bx_diag(&diag, "option '%s' does not take an argument", option.token);
            else if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            return 1;
        }
        switch (option.spec->id) {
            case 'b': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_block_size(option.value, &parsed)
                    || parsed > (uintmax_t)LLONG_MAX) {
                    bx_diag(&diag, "invalid number of bytes: '%s'", option.value);
                    return 1;
                }
                split_size = (long long)parsed;
                split_by_lines = false;
                break;
            }
            case 'l': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_uint(option.value, &parsed)
                    || parsed == 0u
                    || parsed > (uintmax_t)LLONG_MAX) {
                    bx_diag(&diag, "invalid number of lines: '%s'", option.value);
                    return 1;
                }
                lines_per_file = (long long)parsed;
                split_by_lines = true;
                break;
            }
            case 'a': {
                uintmax_t parsed = 0;
                if (!bx_size_parse_uint(option.value, &parsed)
                    || parsed == 0u
                    || parsed > NAME_MAX) {
                    bx_diag(&diag, "invalid suffix length: '%s'", option.value);
                    return 1;
                }
                suffix_length = (int)parsed;
                break;
            }
            case 'd':
                numeric = true;
                hex = false;
                break;
            case 'x':
                hex = true;
                numeric = false;
                break;
            case 'h':
                bx_split_print_help(stdout, progname);
                return 0;
            case 'v':
                bx_cli_print_version(progname);
                return 0;
            default:
                return 1;
        }
    }

    FILE* in = stdin;
    const char* prefix = count == 2 ? operands[1] : "x";
    const char* slash = strrchr(prefix, '/');
    const char* base = slash != NULL ? slash + 1 : prefix;
    if (strlen(base) > (size_t)(NAME_MAX - suffix_length)) {
        bx_diag(&diag, "output file name too long");
        return 1;
    }
    if (count != 0) {
        if (strcmp(operands[0], "-") != 0) {
            in = fopen(operands[0], "r");
            if (!in) {
                bx_perror_path(&diag, operands[0]);
                return 1;
            }
        }
    }

    char* suffix = malloc((size_t)suffix_length + 1);
    char* out_name = malloc(strlen(prefix) + (size_t)suffix_length + 1);
    if (suffix == NULL || out_name == NULL) {
        bx_diag(&diag, "memory exhausted");
        free(suffix);
        free(out_name);
        if (in != stdin)
            fclose(in);
        return 1;
    }
    memset(suffix, numeric || hex ? '0' : 'a', (size_t)suffix_length);
    suffix[suffix_length] = '\0';

    bool eof = false;
    while (!eof) {
        sprintf(out_name, "%s%s", prefix, suffix);
        FILE* out = fopen(out_name, "w");
        if (!out) {
            bx_perror_path(&diag, out_name);
            break;
        }

        bool wrote_anything = false;

        if (split_by_lines) {
            for (long long i = 0; i < lines_per_file; i++) {
                int ch;
                while ((ch = getc(in)) != EOF) {
                    wrote_anything = true;
                    putc(ch, out);
                    if (ch == '\n')
                        break;
                }
                if (ch == EOF) {
                    eof = true;
                    break;
                }
            }
        }
        else {
            for (long long i = 0; i < split_size; i++) {
                int ch = getc(in);
                if (ch == EOF) {
                    eof = true;
                    break;
                }
                wrote_anything = true;
                putc(ch, out);
            }
        }
        fclose(out);

        if (!wrote_anything) {
            unlink(out_name);
        }

        if (eof)
            break;
        next_suffix(suffix, suffix_length, numeric, hex);
    }

    free(suffix);
    free(out_name);
    if (in != stdin)
        fclose(in);

    return 0;
}
