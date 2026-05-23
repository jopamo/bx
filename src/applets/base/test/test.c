#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <time.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

#ifndef S_ISVTX
#define S_ISVTX 01000
#endif

static int pos;
static int argc_val;
static char** argv_val;
static struct bx_diag_ctx* test_diag;
static bool parse_error;

static bool primary(void);
static bool and_expr(void);
static bool or_expr(void);

static void bx_test_print_bracket_help(void) {
    puts("Usage: test EXPRESSION");
    puts("  or:  test");
    puts("  or:  [ EXPRESSION ]");
    puts("  or:  [ ]");
    puts("  or:  [ OPTION");
    puts("Exit with the status determined by EXPRESSION.");
    puts("");
    puts("      --help");
    puts("         display this help and exit");
    puts("      --version");
    puts("         output version information and exit");
    puts("");
    puts("An omitted EXPRESSION defaults to false.  Otherwise,");
    puts("EXPRESSION is true or false and sets exit status.  It is one of:");
    puts("");
    puts("  ( EXPRESSION )               EXPRESSION is true");
    puts("  ! EXPRESSION                 EXPRESSION is false");
    puts("  EXPRESSION1 -a EXPRESSION2   both EXPRESSION1 and EXPRESSION2 are true");
    puts("  EXPRESSION1 -o EXPRESSION2   either EXPRESSION1 or EXPRESSION2 is true");
    puts("");
    puts("  -n STRING            the length of STRING is nonzero");
    puts("  STRING               equivalent to -n STRING");
    puts("  -z STRING            the length of STRING is zero");
    puts("  STRING1 = STRING2    the strings are equal");
    puts("  STRING1 != STRING2   the strings are not equal");
    puts("  STRING1 > STRING2    STRING1 is greater than STRING2 in the current locale");
    puts("  STRING1 < STRING2    STRING1 is less than STRING2 in the current locale");
    puts("");
    puts("  INTEGER1 -eq INTEGER2   INTEGER1 is equal to INTEGER2");
    puts("  INTEGER1 -ge INTEGER2   INTEGER1 is greater than or equal to INTEGER2");
    puts("  INTEGER1 -gt INTEGER2   INTEGER1 is greater than INTEGER2");
    puts("  INTEGER1 -le INTEGER2   INTEGER1 is less than or equal to INTEGER2");
    puts("  INTEGER1 -lt INTEGER2   INTEGER1 is less than INTEGER2");
    puts("  INTEGER1 -ne INTEGER2   INTEGER1 is not equal to INTEGER2");
    puts("");
    puts("  FILE1 -ef FILE2   FILE1 and FILE2 have the same device and inode numbers");
    puts("  FILE1 -nt FILE2   FILE1 is newer (modification date) than FILE2");
    puts("  FILE1 -ot FILE2   FILE1 is older than FILE2");
    puts("");
    puts("  -b FILE     FILE exists and is block special");
    puts("  -c FILE     FILE exists and is character special");
    puts("  -d FILE     FILE exists and is a directory");
    puts("  -e FILE     FILE exists");
    puts("  -f FILE     FILE exists and is a regular file");
    puts("  -g FILE     FILE exists and is set-group-ID");
    puts("  -G FILE     FILE exists and is owned by the effective group ID");
    puts("  -h FILE     FILE exists and is a symbolic link (same as -L)");
    puts("  -k FILE     FILE exists and has its sticky bit set");
    puts("  -L FILE     FILE exists and is a symbolic link (same as -h)");
    puts("  -N FILE     FILE exists and has been modified since it was last read");
    puts("  -O FILE     FILE exists and is owned by the effective user ID");
    puts("  -p FILE     FILE exists and is a named pipe");
    puts("  -r FILE     FILE exists and the user has read access");
    puts("  -s FILE     FILE exists and has a size greater than zero");
    puts("  -S FILE     FILE exists and is a socket");
    puts("  -t FD       file descriptor FD is opened on a terminal");
    puts("  -u FILE     FILE exists and its set-user-ID bit is set");
    puts("  -w FILE     FILE exists and the user has write access");
    puts("  -x FILE     FILE exists and the user has execute (or search) access");
    puts("");
    puts("Except for -h and -L, all FILE-related tests dereference symbolic links.");
    puts("Beware that parentheses need to be escaped (e.g., by backslashes) for shells.");
    puts("INTEGER may also be -l STRING, which evaluates to the length of STRING.");
    puts("");
    puts("Binary -a and -o are ambiguous.  Use 'test EXPR1 && test EXPR2'");
    puts("or 'test EXPR1 || test EXPR2' instead.");
    puts("");
    puts("'[' honors --help and --version, but 'test' treats them as STRINGs.");
    puts("");
    puts("Your shell may have its own version of test and/or [, which usually supersedes");
    puts("the version described here.  Please refer to your shell's documentation");
    puts("for details about the options it supports.");
}

static void bx_test_print_bracket_version(void) {
    printf("[ (bx) %s\n", BX_VERSION);
}

static bool timespec_newer(time_t a_sec, long a_nsec, time_t b_sec, long b_nsec) {
    if (a_sec != b_sec)
        return a_sec > b_sec;
    return a_nsec > b_nsec;
}

static bool stat_mtime_newer(const struct stat* a, const struct stat* b) {
#if defined(__APPLE__)
    return timespec_newer(a->st_mtimespec.tv_sec, a->st_mtimespec.tv_nsec,
                          b->st_mtimespec.tv_sec, b->st_mtimespec.tv_nsec);
#elif defined(st_mtime)
    return timespec_newer(a->st_mtim.tv_sec, a->st_mtim.tv_nsec,
                          b->st_mtim.tv_sec, b->st_mtim.tv_nsec);
#else
    return a->st_mtime > b->st_mtime;
#endif
}

static bool stat_modified_since_read(const struct stat* st) {
#if defined(__APPLE__)
    return timespec_newer(st->st_mtimespec.tv_sec, st->st_mtimespec.tv_nsec,
                          st->st_atimespec.tv_sec, st->st_atimespec.tv_nsec);
#elif defined(st_mtime)
    return timespec_newer(st->st_mtim.tv_sec, st->st_mtim.tv_nsec,
                          st->st_atim.tv_sec, st->st_atim.tv_nsec);
#else
    return st->st_mtime > st->st_atime;
#endif
}

static void mark_parse_error(const char* fmt, ...) {
    if (!parse_error) {
        va_list ap;
        va_start(ap, fmt);
        bx_vdiag(test_diag, fmt, ap);
        va_end(ap);
    }
    parse_error = true;
}

static bool parse_integer_string(const char* text, long long* out) {
    char* end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        mark_parse_error("invalid integer '%s'", text);
        return false;
    }
    *out = value;
    return true;
}

static bool parse_integer_operand_at(int index, long long* out, int* next) {
    if (index < argc_val && strcmp(argv_val[index], "-l") == 0) {
        if (index + 1 >= argc_val) {
            mark_parse_error("missing argument after '-l'");
            return false;
        }
        *out = (long long)strlen(argv_val[index + 1]);
        *next = index + 2;
        return true;
    }

    if (index >= argc_val) {
        mark_parse_error("missing integer");
        return false;
    }

    if (!parse_integer_string(argv_val[index], out))
        return false;
    *next = index + 1;
    return true;
}

static bool is_arithmetic_binary(const char* op) {
    return strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
           strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0 ||
           strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0;
}

static bool eval_arithmetic_binary(long long lhs, const char* op, long long rhs) {
    if (strcmp(op, "-eq") == 0)
        return lhs == rhs;
    if (strcmp(op, "-ne") == 0)
        return lhs != rhs;
    if (strcmp(op, "-gt") == 0)
        return lhs > rhs;
    if (strcmp(op, "-ge") == 0)
        return lhs >= rhs;
    if (strcmp(op, "-lt") == 0)
        return lhs < rhs;
    if (strcmp(op, "-le") == 0)
        return lhs <= rhs;
    return false;
}

static bool is_string_binary(const char* op) {
    return strcmp(op, "=") == 0 || strcmp(op, "==") == 0 ||
           strcmp(op, "!=") == 0 || strcmp(op, ">") == 0 ||
           strcmp(op, "<") == 0;
}

static bool eval_string_binary(const char* lhs, const char* op, const char* rhs) {
    int cmp = strcmp(lhs, rhs);
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return cmp == 0;
    if (strcmp(op, "!=") == 0)
        return cmp != 0;
    if (strcmp(op, ">") == 0)
        return cmp > 0;
    if (strcmp(op, "<") == 0)
        return cmp < 0;
    return false;
}

static bool is_file_binary(const char* op) {
    return strcmp(op, "-ef") == 0 || strcmp(op, "-nt") == 0 ||
           strcmp(op, "-ot") == 0;
}

static bool eval_file_binary(const char* lhs, const char* op, const char* rhs) {
    struct stat left;
    struct stat right;
    bool left_exists = stat(lhs, &left) == 0;
    bool right_exists = stat(rhs, &right) == 0;

    if (strcmp(op, "-ef") == 0) {
        return left_exists && right_exists &&
               left.st_dev == right.st_dev && left.st_ino == right.st_ino;
    }

    if (strcmp(op, "-nt") == 0) {
        if (!left_exists)
            return false;
        if (!right_exists)
            return true;
        return stat_mtime_newer(&left, &right);
    }

    if (strcmp(op, "-ot") == 0) {
        if (!right_exists)
            return false;
        if (!left_exists)
            return true;
        return stat_mtime_newer(&right, &left);
    }

    return false;
}

static bool parse_fd(const char* text, int* fd) {
    long long value;
    if (!parse_integer_string(text, &value))
        return false;
    if (value < 0 || value > INT_MAX) {
        mark_parse_error("invalid file descriptor '%s'", text);
        return false;
    }
    *fd = (int)value;
    return true;
}

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
        case 'a':
            return true;
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
        case 'G':
            return st.st_gid == getegid();
        case 'h':
        case 'L': {
            struct stat lst;
            if (lstat(path, &lst) != 0)
                return false;
            return S_ISLNK(lst.st_mode);
        }
        case 'k':
            return st.st_mode & S_ISVTX;
        case 'N':
            return stat_modified_since_read(&st);
        case 'O':
            return st.st_uid == geteuid();
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
        else {
            mark_parse_error("missing ')'");
        }
        return res;
    }

    if (arg[0] == '-' && arg[1] != '\0' && arg[2] == '\0') {
        if (pos + 1 < argc_val) {
            const char* op = arg;
            const char* val = argv_val[pos + 1];
            pos += 2;
            if (strchr("abcdefgGhLkNOpSsurwx", op[1])) {
                return is_file_type(op, val);
            }
            if (op[1] == 'z')
                return strlen(val) == 0;
            if (op[1] == 'n')
                return strlen(val) > 0;
            if (op[1] == 't') {
                int fd;
                if (!parse_fd(val, &fd))
                    return false;
                return isatty(fd);
            }
            pos -= 2;
        }
    }

    if (pos + 3 < argc_val && strcmp(argv_val[pos], "-l") == 0 &&
        is_arithmetic_binary(argv_val[pos + 2])) {
        long long lhs;
        long long rhs;
        int next;
        if (!parse_integer_operand_at(pos, &lhs, &next))
            return false;
        const char* op = argv_val[next];
        if (!parse_integer_operand_at(next + 1, &rhs, &next))
            return false;
        pos = next;
        return eval_arithmetic_binary(lhs, op, rhs);
    }

    if (pos + 1 < argc_val) {
        const char* s1 = argv_val[pos];
        const char* op = argv_val[pos + 1];
        if (pos + 2 < argc_val) {
            const char* s2 = argv_val[pos + 2];
            if (is_string_binary(op)) {
                pos += 3;
                return eval_string_binary(s1, op, s2);
            }
            if (is_file_binary(op)) {
                pos += 3;
                return eval_file_binary(s1, op, s2);
            }
            if (is_arithmetic_binary(op)) {
                long long lhs;
                long long rhs;
                int next;
                if (!parse_integer_operand_at(pos, &lhs, &next))
                    return false;
                if (next >= argc_val || strcmp(argv_val[next], op) != 0) {
                    mark_parse_error("invalid expression");
                    return false;
                }
                if (!parse_integer_operand_at(next + 1, &rhs, &next))
                    return false;
                pos = next;
                return eval_arithmetic_binary(lhs, op, rhs);
            }
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
    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "test");
    bool bracket = (progname[0] == '[' && progname[1] == '\0');

    if (bracket && argc == 2 && strcmp(argv[1], "--help") == 0) {
        bx_test_print_bracket_help();
        return 0;
    }

    if (bracket && argc == 2 && strcmp(argv[1], "--version") == 0) {
        bx_test_print_bracket_version();
        return 0;
    }

    if (bracket) {
        if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
            fprintf(stderr, "%s: missing ']'\n", progname);
            return 2;
        }
        argc--;
    }

    if (argc == 1)
        return 1;

    pos = 1;
    argc_val = argc;
    argv_val = argv;
    struct bx_diag_ctx diag = {.progname = progname, .exit_status = 0};
    test_diag = &diag;
    parse_error = false;

    bool result = or_expr();
    if (!parse_error && pos < argc_val) {
        mark_parse_error("extra argument '%s'", argv_val[pos]);
    }

    if (parse_error)
        return 2;
    return result ? 0 : 1;
}
