#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "applets.h"

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
                        case 'x': {
                            int val = 0;
                            int v;
                            if ((v = hex_val(p[1])) != -1) {
                                val = v;
                                p++;
                                if ((v = hex_val(p[1])) != -1) {
                                    val = val * 16 + v;
                                    p++;
                                }
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
