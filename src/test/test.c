#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <errno.h>
#include "applets.h"
#include "diag.h"

static int pos;
static int argc_val;
static char** argv_val;

static bool primary(void);
static bool and_expr(void);
static bool or_expr(void);

static bool is_file_type(const char* op, const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        if (op[1] == 'h' || op[1] == 'L') {
            if (lstat(path, &st) != 0)
                return false;
        }
        else {
            return false;
        }
    }

    switch (op[1]) {
        case 'b':
            return S_ISBLK(st.st_mode);
        case 'c':
            return S_ISCHR(st.st_mode);
        case 'd':
            return S_ISDIR(st.st_mode);
        case 'e':
            return true;
        case 'f':
            return S_ISREG(st.st_mode);
        case 'g':
            return st.st_mode & S_ISGID;
        case 'h':
        case 'L': {
            struct stat lst;
            if (lstat(path, &lst) != 0)
                return false;
            return S_ISLNK(lst.st_mode);
        }
        case 'p':
            return S_ISFIFO(st.st_mode);
        case 'r':
            return access(path, R_OK) == 0;
        case 's':
            return st.st_size > 0;
        case 'S':
            return S_ISSOCK(st.st_mode);
        case 'u':
            return st.st_mode & S_ISUID;
        case 'w':
            return access(path, W_OK) == 0;
        case 'x':
            return access(path, X_OK) == 0;
        default:
            return false;
    }
}

static bool primary(void) {
    if (pos >= argc_val)
        return false;

    const char* arg = argv_val[pos];
    if (strcmp(arg, "!") == 0) {
        pos++;
        return !primary();
    }
    if (strcmp(arg, "(") == 0) {
        pos++;
        bool res = or_expr();
        if (pos < argc_val && strcmp(argv_val[pos], ")") == 0) {
            pos++;
        }
        return res;
    }

    if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0') {
        if (pos + 1 < argc_val) {
            const char* op = arg;
            const char* val = argv_val[pos + 1];
            pos += 2;
            if (strchr("bcdefghLpSsuw r x", op[1])) {
                return is_file_type(op, val);
            }
            if (op[1] == 'z')
                return strlen(val) == 0;
            if (op[1] == 'n')
                return strlen(val) > 0;
            if (op[1] == 't')
                return isatty(atoi(val));
        }
    }

    if (pos + 1 < argc_val) {
        const char* s1 = argv_val[pos];
        const char* op = argv_val[pos + 1];
        if (pos + 2 < argc_val) {
            const char* s2 = argv_val[pos + 2];
            pos += 3;
            if (strcmp(op, "=") == 0)
                return strcmp(s1, s2) == 0;
            if (strcmp(op, "!=") == 0)
                return strcmp(s1, s2) != 0;

            long long v1 = atoll(s1);
            long long v2 = atoll(s2);
            if (strcmp(op, "-eq") == 0)
                return v1 == v2;
            if (strcmp(op, "-ne") == 0)
                return v1 != v2;
            if (strcmp(op, "-gt") == 0)
                return v1 > v2;
            if (strcmp(op, "-ge") == 0)
                return v1 >= v2;
            if (strcmp(op, "-lt") == 0)
                return v1 < v2;
            if (strcmp(op, "-le") == 0)
                return v1 <= v2;
        }
    }

    pos++;
    return strlen(arg) > 0;
}

static bool and_expr(void) {
    bool res = primary();
    while (pos < argc_val && strcmp(argv_val[pos], "-a") == 0) {
        pos++;
        res = primary() && res;  // Simple evaluation
    }
    return res;
}

static bool or_expr(void) {
    bool res = and_expr();
    while (pos < argc_val && strcmp(argv_val[pos], "-o") == 0) {
        pos++;
        res = and_expr() || res;
    }
    return res;
}

int bx_test_main(int argc, char** argv) {
    const char* progname = argv[0];
    bool bracket = (progname[0] == '[' && progname[1] == '\0');

    if (bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            bx_err("missing ']'");
            return 2;
        }
        argc--;
    }

    if (argc == 1)
        return 1;

    pos = 1;
    argc_val = argc;
    argv_val = argv;

    return or_expr() ? 0 : 1;
}
