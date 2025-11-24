#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "applets.h"
#include "lib/cli_common.h"

static void bx_echo_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [SHORT-OPTION]... [STRING]...\n", progname);
    fprintf(stream, "  or:  %s LONG-OPTION\n", progname);
    fprintf(stream, "Display a line of text.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -n     do not output the trailing newline\n");
    fprintf(stream, "  -e     enable interpretation of backslash escapes\n");
    fprintf(stream, "  -E     disable interpretation of backslash escapes (default)\n");
    fprintf(stream, "      --help\n");
    fprintf(stream, "         display this help and exit\n");
    fprintf(stream, "      --version\n");
    fprintf(stream, "         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "If -e is in effect, the following sequences are recognized:\n");
    fprintf(stream, "\n");
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
    fprintf(stream, "  \\0NNN   byte with octal value NNN (1 to 3 digits)\n");
    fprintf(stream, "  \\xHH    byte with hexadecimal value HH (1 to 2 digits)\n");
    fprintf(stream, "\n");
    fprintf(stream, "Your shell may have its own version of echo, which usually supersedes\n");
    fprintf(stream, "the version described here. Please refer to your shell's documentation\n");
    fprintf(stream, "for details about the options it supports.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Consider using the printf(1) command instead, as it avoids problems when\n");
    fprintf(stream, "outputting option-like strings.\n");
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int bx_echo_main(int argc, char** argv) {
    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "echo");

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        bx_echo_print_help(stdout, progname);
        return 0;
    }

    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        bx_cli_print_version(progname);
        return 0;
    }

    bool n_flag = false;
    bool e_flag = false;

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        const char* p = argv[i] + 1;
        if (*p == '\0')
            break;

        bool all_valid = true;
        bool cur_n = false, cur_e = false, cur_E = false;
        for (const char* c = p; *c; c++) {
            if (*c == 'n')
                cur_n = true;
            else if (*c == 'e') {
                cur_e = true;
                cur_E = false;
            }
            else if (*c == 'E') {
                cur_E = true;
                cur_e = false;
            }
            else {
                all_valid = false;
                break;
            }
        }

        if (!all_valid)
            break;
        if (cur_n)
            n_flag = true;
        if (cur_e)
            e_flag = true;
        if (cur_E)
            e_flag = false;
        i++;
    }

    for (; i < argc; i++) {
        const char* s = argv[i];
        if (e_flag) {
            for (const char* p = s; *p; p++) {
                if (*p == '\\' && *(p + 1)) {
                    p++;
                    switch (*p) {
                        case '\\':
                            putchar('\\');
                            break;
                        case 'a':
                            putchar('\a');
                            break;
                        case 'b':
                            putchar('\b');
                            break;
                        case 'c':
                            return 0;  // stop output
                        case 'e':
                            putchar('\033');
                            break;
                        case 'f':
                            putchar('\f');
                            break;
                        case 'n':
                            putchar('\n');
                            break;
                        case 'r':
                            putchar('\r');
                            break;
                        case 't':
                            putchar('\t');
                            break;
                        case 'v':
                            putchar('\v');
                            break;
                        case '0': {
                            int val = 0;
                            for (int j = 0; j < 3 && p[1] >= '0' && p[1] <= '7'; j++) {
                                val = val * 8 + (*(++p) - '0');
                            }
                            putchar(val);
                            break;
                        }
                        case '1':
                        case '2':
                        case '3':
                        case '4':
                        case '5':
                        case '6':
                        case '7': {
                            int val = *p - '0';
                            for (int j = 0; j < 2 && p[1] >= '0' && p[1] <= '7'; j++) {
                                val = val * 8 + (*(++p) - '0');
                            }
                            putchar(val);
                            break;
                        }
                        case 'x': {
                            int val = 0;
                            int v;
                            if ((v = hex_val(p[1])) == -1) {
                                putchar('\\');
                                putchar('x');
                                break;
                            }

                            val = v;
                            p++;
                            if ((v = hex_val(p[1])) != -1) {
                                val = val * 16 + v;
                                p++;
                            }
                            putchar(val);
                            break;
                        }
                        default:
                            putchar('\\');
                            putchar(*p);
                            break;
                    }
                }
                else {
                    putchar(*p);
                }
            }
        }
        else {
            fputs(s, stdout);
        }
        if (i + 1 < argc)
            putchar(' ');
    }

    if (!n_flag)
        putchar('\n');

    return 0;
}
