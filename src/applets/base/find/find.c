#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "search/walk.h"

struct find_opts {
    bool depth_first;
    int max_depth;
    int min_depth;
    bool follow_symlinks;
    bool follow_root_symlink;
};

enum find_expr_kind {
    FIND_EXPR_TRUE,
    FIND_EXPR_FALSE,
    FIND_EXPR_NAME,
    FIND_EXPR_PATH,
    FIND_EXPR_LNAME,
    FIND_EXPR_TYPE,
    FIND_EXPR_INUM,
    FIND_EXPR_LINKS,
    FIND_EXPR_UID,
    FIND_EXPR_GID,
    FIND_EXPR_USER,
    FIND_EXPR_GROUP,
    FIND_EXPR_NOUSER,
    FIND_EXPR_NOGROUP,
    FIND_EXPR_PERM,
    FIND_EXPR_SIZE,
    FIND_EXPR_AMIN,
    FIND_EXPR_ATIME,
    FIND_EXPR_CMIN,
    FIND_EXPR_CTIME,
    FIND_EXPR_MMIN,
    FIND_EXPR_MTIME,
    FIND_EXPR_USED,
    FIND_EXPR_ANEWER,
    FIND_EXPR_CNEWER,
    FIND_EXPR_NEWER,
    FIND_EXPR_EMPTY,
    FIND_EXPR_READABLE,
    FIND_EXPR_WRITABLE,
    FIND_EXPR_EXECUTABLE,
    FIND_EXPR_PRINT,
    FIND_EXPR_PRINT0,
    FIND_EXPR_FPRINT,
    FIND_EXPR_FPRINT0,
    FIND_EXPR_DELETE,
    FIND_EXPR_QUIT,
    FIND_EXPR_NOT,
    FIND_EXPR_AND,
    FIND_EXPR_OR,
    FIND_EXPR_COMMA,
};

struct find_expr {
    enum find_expr_kind kind;
    struct find_expr *left;
    struct find_expr *right;
    const char *text;
    char type_filter;
    bool ignore_case;
    long long number;
    int number_cmp;
    mode_t perm_bits;
    int perm_kind;
    unsigned long long size_unit;
    struct timespec ref_time;
};

struct find_parser {
    const char *progname;
    char **argv;
    int argc;
    int pos;
    bool explicit_action;
    struct find_opts *opts;
};

struct find_state {
    const char *progname;
    struct find_opts *opts;
    struct find_expr *expr;
    bool *stop;
    int status;
    struct timespec now;
};

static void find_report_error(const char *progname, const char *path, int errnum) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void find_print_help(const char *progname) {
    printf("Usage: %s [PATH]... [EXPRESSION]\n", progname);
    puts("Search for files in a directory hierarchy.");
    puts("");
    puts("  -H            follow command-line symlinks");
    puts("  -L            follow all symlinks");
    puts("  -P            never follow symlinks");
    puts("  -depth        process directory contents before the directory");
    puts("  -maxdepth N   descend at most N levels below the roots");
    puts("  -mindepth N   do not act on levels less than N");
    puts("  -name PATTERN match basename against PATTERN");
    puts("  -lname PATTERN match symlink target against PATTERN");
    puts("  -type [fdl]   match file type");
    puts("  -inum N       match inode number");
    puts("  -links N      match link count");
    puts("  -uid N        match user id");
    puts("  -gid N        match group id");
    puts("  -user NAME    match user name or numeric uid");
    puts("  -group NAME   match group name or numeric gid");
    puts("  -nouser       match files whose uid has no passwd entry");
    puts("  -nogroup      match files whose gid has no group entry");
    puts("  -perm MODE    match permission bits");
    puts("  -size N[cwbkMG]  match file size");
    puts("  -amin N       match access age in minutes");
    puts("  -atime N      match access age in 24-hour days");
    puts("  -cmin N       match status-change age in minutes");
    puts("  -ctime N      match status-change age in 24-hour days");
    puts("  -mmin N       match modification age in minutes");
    puts("  -mtime N      match modification age in 24-hour days");
    puts("  -used N       match access age measured from last status change");
    puts("  -anewer FILE  match entries accessed more recently than FILE was modified");
    puts("  -cnewer FILE  match entries changed more recently than FILE was modified");
    puts("  -newer FILE   match entries newer than FILE");
    puts("  -true         always true");
    puts("  -false        always false");
    puts("  -print        print path");
    puts("  -print0       print path followed by NUL");
    puts("  -fprint FILE  write path to FILE");
    puts("  -fprint0 FILE write path followed by NUL to FILE");
    puts("  -delete       delete matched entries");
    puts("  -quit         stop after the first deciding result");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

static void find_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool parse_int_arg(const char *progname, const char *optname, const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > 1<<20) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool find_parse_numeric_test(const char *progname, const char *optname,
                                    const char *text, long long *value, int *cmp) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }

    *cmp = 0;
    if (*text == '+') {
        *cmp = 1;
        text++;
    } else if (*text == '-') {
        *cmp = -1;
        text++;
    }

    char *end = NULL;
    errno = 0;
    long long v = strtoll(text, &end, 10);
    if (*text == '\0' || !end || *end != '\0' || errno != 0 || v < 0) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }
    *value = v;
    return true;
}

static bool find_numeric_match(unsigned long long actual, long long expected, int cmp) {
    unsigned long long want = (unsigned long long)expected;
    if (cmp > 0)
        return actual > want;
    if (cmp < 0)
        return actual < want;
    return actual == want;
}

static bool find_parse_unsigned_id(const char *text, unsigned long long *value) {
    if (!text || *text == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!isdigit(*p))
            return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;
    *value = v;
    return true;
}

static bool find_parse_user_id(const char *progname, const char *text, long long *value) {
    struct passwd *pw = getpwnam(text);
    if (pw) {
        *value = (long long)pw->pw_uid;
        return true;
    }

    unsigned long long numeric = 0;
    if (find_parse_unsigned_id(text, &numeric)) {
        *value = (long long)numeric;
        return true;
    }

    fprintf(stderr, "%s: invalid user name or UID argument to -user: %s\n", progname, text);
    return false;
}

static bool find_parse_group_id(const char *progname, const char *text, long long *value) {
    struct group *gr = getgrnam(text);
    if (gr) {
        *value = (long long)gr->gr_gid;
        return true;
    }

    unsigned long long numeric = 0;
    if (find_parse_unsigned_id(text, &numeric)) {
        *value = (long long)numeric;
        return true;
    }

    fprintf(stderr, "%s: invalid group name or GID argument to -group: %s\n", progname, text);
    return false;
}

static bool find_parse_perm(const char *progname, const char *text, mode_t *bits, int *kind) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text ? text : "(null)");
        return false;
    }

    *kind = 0;
    if (*text == '-') {
        *kind = 1;
        text++;
    } else if (*text == '/') {
        *kind = 2;
        text++;
    }

    if (*text == '\0') {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text);
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < '0' || *p > '7') {
            fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text);
            return false;
        }
    }

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 8);
    if (errno != 0 || !end || *end != '\0' || value > 07777u) {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text);
        return false;
    }

    *bits = (mode_t)value;
    if (*kind == 2 && *bits == 0) {
        fprintf(stderr,
                "%s: warning: you have specified a mode pattern /000 (which is equivalent to /000). "
                "The meaning of -perm /000 has now been changed to be consistent with -perm -000; "
                "that is, while it used to match no files, it now matches all files.\n",
                progname);
    }
    return true;
}

static bool find_perm_match(mode_t mode, mode_t bits, int kind) {
    mode_t actual = mode & 07777u;
    switch (kind) {
    case 0:
        return actual == bits;
    case 1:
        return (actual & bits) == bits;
    case 2:
        return bits == 0 ? true : (actual & bits) != 0;
    default:
        return false;
    }
}

static bool find_parse_size_arg(const char *progname, const char *text,
                                long long *value, int *cmp, unsigned long long *unit) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to -size: %s\n", progname, text ? text : "(null)");
        return false;
    }

    *cmp = 0;
    if (*text == '+') {
        *cmp = 1;
        text++;
    } else if (*text == '-') {
        *cmp = -1;
        text++;
    }

    char *end = NULL;
    errno = 0;
    long long v = strtoll(text, &end, 10);
    if (*text == '\0' || !end || errno != 0 || v < 0) {
        fprintf(stderr, "%s: invalid argument to -size: %s\n", progname, text ? text : "(null)");
        return false;
    }

    unsigned long long u = 512;
    if (*end != '\0') {
        if (end[1] != '\0') {
            fprintf(stderr, "%s: invalid argument to -size: %s\n", progname, text);
            return false;
        }
        switch (*end) {
        case 'b': u = 512; break;
        case 'c': u = 1; break;
        case 'w': u = 2; break;
        case 'k': u = 1024; break;
        case 'M': u = 1024ULL * 1024ULL; break;
        case 'G': u = 1024ULL * 1024ULL * 1024ULL; break;
        default:
            fprintf(stderr, "%s: invalid argument to -size: %s\n", progname, text);
            return false;
        }
    }

    *value = v;
    *unit = u;
    return true;
}

static bool find_size_match(off_t size, long long expected, int cmp, unsigned long long unit) {
    unsigned long long bytes = size < 0 ? 0 : (unsigned long long)size;
    unsigned long long quanta = unit == 0 ? 0 : (bytes + unit - 1) / unit;
    return find_numeric_match(quanta, expected, cmp);
}

static bool find_parse_newer_ref(const char *progname, const char *path,
                                 bool follow_root_symlink, struct timespec *out) {
    struct stat st;
    int rc = follow_root_symlink ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) {
        find_report_error(progname, path, errno);
        return false;
    }
    *out = st.st_mtim;
    return true;
}

static int find_timespec_cmp(struct timespec lhs, struct timespec rhs) {
    if (lhs.tv_sec != rhs.tv_sec)
        return lhs.tv_sec < rhs.tv_sec ? -1 : 1;
    if (lhs.tv_nsec != rhs.tv_nsec)
        return lhs.tv_nsec < rhs.tv_nsec ? -1 : 1;
    return 0;
}

static bool find_time_age_match(struct timespec now, struct timespec when,
                                long long expected, int cmp,
                                unsigned long long unit_seconds) {
    time_t sec = now.tv_sec - when.tv_sec;
    long nsec = now.tv_nsec - when.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    unsigned long long age = 0;
    if (sec > 0 && unit_seconds > 0)
        age = (unsigned long long)sec / unit_seconds;
    return find_numeric_match(age, expected, cmp);
}

static bool find_used_match(struct timespec atime, struct timespec ctime,
                            long long expected, int cmp) {
    time_t sec = atime.tv_sec - ctime.tv_sec;
    long nsec = atime.tv_nsec - ctime.tv_nsec;
    if (nsec < 0) {
        sec--;
        nsec += 1000000000L;
    }
    if (sec < 0 || (sec == 0 && nsec <= 0))
        return false;

    unsigned long long days = (unsigned long long)(sec / 86400ULL);
    if ((sec % 86400ULL) != 0 || nsec != 0)
        days++;
    return find_numeric_match(days, expected, cmp);
}

static const char *find_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool find_match_pattern(const char *pattern, const char *text, bool ignore_case) {
    return fnmatch(pattern, text, ignore_case ? FNM_CASEFOLD : 0) == 0;
}

static bool find_match_link_target(const struct walk_entry *entry, const char *pattern, bool ignore_case) {
    if (!S_ISLNK(entry->mode))
        return false;

    char buf[PATH_MAX + 1];
    ssize_t len = readlink(entry->path, buf, PATH_MAX);
    if (len < 0)
        return false;
    buf[len] = '\0';
    return find_match_pattern(pattern, buf, ignore_case);
}

static bool find_write_path_file(const char *progname, const char *filename, const char *path, char terminator) {
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        find_report_error(progname, filename, errno);
        return false;
    }
    size_t path_len = strlen(path);
    bool ok = fwrite(path, 1, path_len, fp) == path_len && fputc(terminator, fp) != EOF;
    if (!ok)
        find_report_error(progname, filename, errno ? errno : EIO);
    fclose(fp);
    return ok;
}

static bool find_matches_type(const struct walk_entry *entry, char type_filter) {
    switch (type_filter) {
    case 'f':
        return S_ISREG(entry->mode);
    case 'd':
        return S_ISDIR(entry->mode);
    case 'l':
        return S_ISLNK(entry->mode);
    case 'p':
        return S_ISFIFO(entry->mode);
    case 's':
        return S_ISSOCK(entry->mode);
    case 'b':
        return S_ISBLK(entry->mode);
    case 'c':
        return S_ISCHR(entry->mode);
    default:
        return false;
    }
}

static bool find_is_empty(const struct walk_entry *entry) {
    if (S_ISREG(entry->mode)) {
        struct stat st;
        if (stat(entry->path, &st) != 0)
            return false;
        return st.st_size == 0;
    }
    if (!S_ISDIR(entry->mode))
        return false;

    DIR *dir = opendir(entry->path);
    if (!dir)
        return false;
    struct dirent *ent;
    bool empty = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    return empty;
}

static struct find_expr *find_expr_new(enum find_expr_kind kind) {
    struct find_expr *expr = calloc(1, sizeof(*expr));
    if (!expr)
        return NULL;
    expr->kind = kind;
    return expr;
}

static void find_expr_free(struct find_expr *expr) {
    if (!expr)
        return;
    find_expr_free(expr->left);
    find_expr_free(expr->right);
    free(expr);
}

static bool token_starts_expression(const char *arg) {
    if (!arg || arg[0] == '\0')
        return false;
    return arg[0] == '-' || arg[0] == '!' || arg[0] == '(' || arg[0] == ')'|| strcmp(arg, ",") == 0;
}

static bool find_is_and_token(const char *arg) {
    return arg && (strcmp(arg, "-a") == 0 || strcmp(arg, "-and") == 0);
}

static bool find_is_or_token(const char *arg) {
    return arg && (strcmp(arg, "-o") == 0 || strcmp(arg, "-or") == 0);
}

static bool find_is_not_token(const char *arg) {
    return arg && (strcmp(arg, "!") == 0 || strcmp(arg, "-not") == 0);
}

static bool find_is_primary_start(const char *arg) {
    if (!arg)
        return false;
    if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0)
        return false;
    if (find_is_and_token(arg) || find_is_or_token(arg))
        return false;
    return token_starts_expression(arg);
}

static struct find_expr *find_parse_expr(struct find_parser *parser);

static struct find_expr *find_make_binary(enum find_expr_kind kind,
                                          struct find_expr *left,
                                          struct find_expr *right) {
    struct find_expr *expr = find_expr_new(kind);
    if (!expr) {
        find_expr_free(left);
        find_expr_free(right);
        return NULL;
    }
    expr->left = left;
    expr->right = right;
    return expr;
}

static struct find_expr *find_parse_primary(struct find_parser *parser) {
    if (parser->pos >= parser->argc) {
        fprintf(stderr, "%s: expected an expression\n", parser->progname);
        return NULL;
    }

    const char *arg = parser->argv[parser->pos++];
    if (strcmp(arg, "(") == 0) {
        struct find_expr *expr = find_parse_expr(parser);
        if (!expr)
            return NULL;
        if (parser->pos >= parser->argc || strcmp(parser->argv[parser->pos], ")") != 0) {
            fprintf(stderr, "%s: expected ')'\n", parser->progname);
            find_expr_free(expr);
            return NULL;
        }
        parser->pos++;
        return expr;
    }

    struct find_expr *expr = NULL;
    if (strcmp(arg, "-true") == 0) {
        expr = find_expr_new(FIND_EXPR_TRUE);
    } else if (strcmp(arg, "-false") == 0) {
        expr = find_expr_new(FIND_EXPR_FALSE);
    } else if (strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_NAME);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iname") == 0;
        }
    } else if (strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 || strcmp(arg, "-iwholename") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_PATH);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iwholename") == 0;
        }
    } else if (strcmp(arg, "-lname") == 0 || strcmp(arg, "-ilname") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_LNAME);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-ilname") == 0;
        }
    } else if (strcmp(arg, "-type") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-type'\n", parser->progname);
            return NULL;
        }
        const char *type_arg = parser->argv[parser->pos++];
        if (type_arg[0] == '\0' || type_arg[1] != '\0' ||
            (type_arg[0] != 'f' && type_arg[0] != 'd' && type_arg[0] != 'l' &&
             type_arg[0] != 'p' && type_arg[0] != 's' && type_arg[0] != 'b' &&
             type_arg[0] != 'c')) {
            fprintf(stderr, "%s: unknown argument to -type: %s\n", parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_TYPE);
        if (expr)
            expr->type_filter = type_arg[0];
    } else if (strcmp(arg, "-inum") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-inum'\n", parser->progname);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_INUM);
        if (expr && !find_parse_numeric_test(parser->progname, "-inum", parser->argv[parser->pos],
                                             &expr->number, &expr->number_cmp)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-links") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-links'\n", parser->progname);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_LINKS);
        if (expr && !find_parse_numeric_test(parser->progname, "-links", parser->argv[parser->pos],
                                             &expr->number, &expr->number_cmp)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-uid") == 0 || strcmp(arg, "-gid") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(strcmp(arg, "-uid") == 0 ? FIND_EXPR_UID : FIND_EXPR_GID);
        if (expr && !find_parse_numeric_test(parser->progname, arg, parser->argv[parser->pos],
                                             &expr->number, &expr->number_cmp)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-user") == 0 || strcmp(arg, "-group") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(strcmp(arg, "-user") == 0 ? FIND_EXPR_USER : FIND_EXPR_GROUP);
        bool ok = false;
        if (expr && strcmp(arg, "-user") == 0)
            ok = find_parse_user_id(parser->progname, parser->argv[parser->pos], &expr->number);
        else if (expr)
            ok = find_parse_group_id(parser->progname, parser->argv[parser->pos], &expr->number);
        if (expr && !ok) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-nouser") == 0) {
        expr = find_expr_new(FIND_EXPR_NOUSER);
    } else if (strcmp(arg, "-nogroup") == 0) {
        expr = find_expr_new(FIND_EXPR_NOGROUP);
    } else if (strcmp(arg, "-perm") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-perm'\n", parser->progname);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_PERM);
        if (expr && !find_parse_perm(parser->progname, parser->argv[parser->pos],
                                     &expr->perm_bits, &expr->perm_kind)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-size") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-size'\n", parser->progname);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_SIZE);
        if (expr && !find_parse_size_arg(parser->progname, parser->argv[parser->pos],
                                         &expr->number, &expr->number_cmp, &expr->size_unit)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-amin") == 0 || strcmp(arg, "-atime") == 0 ||
               strcmp(arg, "-cmin") == 0 || strcmp(arg, "-ctime") == 0 ||
               strcmp(arg, "-mmin") == 0 || strcmp(arg, "-mtime") == 0 ||
               strcmp(arg, "-used") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        enum find_expr_kind kind = FIND_EXPR_AMIN;
        if (strcmp(arg, "-atime") == 0)
            kind = FIND_EXPR_ATIME;
        else if (strcmp(arg, "-cmin") == 0)
            kind = FIND_EXPR_CMIN;
        if (strcmp(arg, "-ctime") == 0)
            kind = FIND_EXPR_CTIME;
        else if (strcmp(arg, "-mmin") == 0)
            kind = FIND_EXPR_MMIN;
        else if (strcmp(arg, "-mtime") == 0)
            kind = FIND_EXPR_MTIME;
        else if (strcmp(arg, "-used") == 0)
            kind = FIND_EXPR_USED;
        expr = find_expr_new(kind);
        if (expr && !find_parse_numeric_test(parser->progname, arg, parser->argv[parser->pos],
                                             &expr->number, &expr->number_cmp)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-anewer") == 0 || strcmp(arg, "-cnewer") == 0 ||
               strcmp(arg, "-newer") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        enum find_expr_kind kind = FIND_EXPR_NEWER;
        if (strcmp(arg, "-anewer") == 0)
            kind = FIND_EXPR_ANEWER;
        else if (strcmp(arg, "-cnewer") == 0)
            kind = FIND_EXPR_CNEWER;
        expr = find_expr_new(kind);
        if (expr && !find_parse_newer_ref(parser->progname, parser->argv[parser->pos],
                                          parser->opts && parser->opts->follow_root_symlink,
                                          &expr->ref_time)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-empty") == 0) {
        expr = find_expr_new(FIND_EXPR_EMPTY);
    } else if (strcmp(arg, "-readable") == 0) {
        expr = find_expr_new(FIND_EXPR_READABLE);
    } else if (strcmp(arg, "-writable") == 0) {
        expr = find_expr_new(FIND_EXPR_WRITABLE);
    } else if (strcmp(arg, "-executable") == 0) {
        expr = find_expr_new(FIND_EXPR_EXECUTABLE);
    } else if (strcmp(arg, "-print") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_PRINT);
    } else if (strcmp(arg, "-print0") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_PRINT0);
    } else if (strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(strcmp(arg, "-fprint") == 0 ? FIND_EXPR_FPRINT : FIND_EXPR_FPRINT0);
        if (expr)
            expr->text = parser->argv[parser->pos++];
    } else if (strcmp(arg, "-delete") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_DELETE);
    } else if (strcmp(arg, "-quit") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_QUIT);
    } else {
        fprintf(stderr, "%s: unknown predicate `%s'\n", parser->progname, arg);
        return NULL;
    }

    if (!expr)
        fprintf(stderr, "%s: out of memory\n", parser->progname);
    return expr;
}

static struct find_expr *find_parse_not(struct find_parser *parser) {
    if (parser->pos < parser->argc && find_is_not_token(parser->argv[parser->pos])) {
        parser->pos++;
        struct find_expr *child = find_parse_not(parser);
        if (!child)
            return NULL;
        struct find_expr *expr = find_expr_new(FIND_EXPR_NOT);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            find_expr_free(child);
            return NULL;
        }
        expr->left = child;
        return expr;
    }
    return find_parse_primary(parser);
}

static struct find_expr *find_parse_and(struct find_parser *parser) {
    struct find_expr *expr = find_parse_not(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc) {
        const char *arg = parser->argv[parser->pos];
        if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0 || find_is_or_token(arg))
            break;
        if (find_is_and_token(arg))
            parser->pos++;
        else if (!find_is_primary_start(arg))
            break;

        struct find_expr *rhs = find_parse_not(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_AND, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static struct find_expr *find_parse_or(struct find_parser *parser) {
    struct find_expr *expr = find_parse_and(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc && find_is_or_token(parser->argv[parser->pos])) {
        parser->pos++;
        struct find_expr *rhs = find_parse_and(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_OR, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static struct find_expr *find_parse_expr(struct find_parser *parser) {
    struct find_expr *expr = find_parse_or(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], ",") == 0) {
        parser->pos++;
        struct find_expr *rhs = find_parse_or(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_COMMA, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static bool find_eval_expr(const struct find_expr *expr, const struct walk_entry *entry,
                           struct find_state *st) {
    if (!expr)
        return true;
    if (st->stop && *st->stop)
        return false;

    switch (expr->kind) {
    case FIND_EXPR_TRUE:
        return true;
    case FIND_EXPR_FALSE:
        return false;
    case FIND_EXPR_NAME:
        return find_match_pattern(expr->text, find_basename(entry->path), expr->ignore_case);
    case FIND_EXPR_PATH:
        return find_match_pattern(expr->text, entry->path, expr->ignore_case);
    case FIND_EXPR_LNAME:
        return find_match_link_target(entry, expr->text, expr->ignore_case);
    case FIND_EXPR_TYPE:
        return find_matches_type(entry, expr->type_filter);
    case FIND_EXPR_INUM:
        return find_numeric_match((unsigned long long)entry->inode, expr->number, expr->number_cmp);
    case FIND_EXPR_LINKS:
        return find_numeric_match((unsigned long long)entry->nlink, expr->number, expr->number_cmp);
    case FIND_EXPR_UID:
        return find_numeric_match((unsigned long long)entry->uid, expr->number, expr->number_cmp);
    case FIND_EXPR_GID:
        return find_numeric_match((unsigned long long)entry->gid, expr->number, expr->number_cmp);
    case FIND_EXPR_USER:
        return find_numeric_match((unsigned long long)entry->uid, expr->number, 0);
    case FIND_EXPR_GROUP:
        return find_numeric_match((unsigned long long)entry->gid, expr->number, 0);
    case FIND_EXPR_NOUSER:
        return getpwuid(entry->uid) == NULL;
    case FIND_EXPR_NOGROUP:
        return getgrgid(entry->gid) == NULL;
    case FIND_EXPR_PERM:
        return find_perm_match(entry->mode, expr->perm_bits, expr->perm_kind);
    case FIND_EXPR_SIZE:
        return find_size_match(entry->size, expr->number, expr->number_cmp, expr->size_unit);
    case FIND_EXPR_AMIN:
        return find_time_age_match(st->now, entry->atime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_ATIME:
        return find_time_age_match(st->now, entry->atime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_CMIN:
        return find_time_age_match(st->now, entry->ctime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_CTIME:
        return find_time_age_match(st->now, entry->ctime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_MMIN:
        return find_time_age_match(st->now, entry->mtime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_MTIME:
        return find_time_age_match(st->now, entry->mtime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_USED:
        return find_used_match(entry->atime, entry->ctime, expr->number, expr->number_cmp);
    case FIND_EXPR_ANEWER:
        return find_timespec_cmp(entry->atime, expr->ref_time) > 0;
    case FIND_EXPR_CNEWER:
        return find_timespec_cmp(entry->ctime, expr->ref_time) > 0;
    case FIND_EXPR_NEWER:
        return find_timespec_cmp(entry->mtime, expr->ref_time) > 0;
    case FIND_EXPR_EMPTY:
        return find_is_empty(entry);
    case FIND_EXPR_READABLE:
        return access(entry->path, R_OK) == 0;
    case FIND_EXPR_WRITABLE:
        return access(entry->path, W_OK) == 0;
    case FIND_EXPR_EXECUTABLE:
        return access(entry->path, X_OK) == 0;
    case FIND_EXPR_PRINT:
        printf("%s\n", entry->path);
        return true;
    case FIND_EXPR_PRINT0:
        printf("%s%c", entry->path, '\0');
        return true;
    case FIND_EXPR_FPRINT:
        if (!find_write_path_file(st->progname, expr->text, entry->path, '\n')) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_FPRINT0:
        if (!find_write_path_file(st->progname, expr->text, entry->path, '\0')) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_DELETE:
        if (strcmp(entry->path, ".") == 0) {
            errno = EBUSY;
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        if ((entry->is_dir ? rmdir(entry->path) : unlink(entry->path)) != 0) {
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_QUIT:
        if (st->stop)
            *st->stop = true;
        return true;
    case FIND_EXPR_NOT:
        return !find_eval_expr(expr->left, entry, st);
    case FIND_EXPR_AND: {
        bool lhs = find_eval_expr(expr->left, entry, st);
        if (!lhs || (st->stop && *st->stop))
            return lhs;
        return find_eval_expr(expr->right, entry, st);
    }
    case FIND_EXPR_OR: {
        bool lhs = find_eval_expr(expr->left, entry, st);
        if (lhs || (st->stop && *st->stop))
            return lhs;
        return find_eval_expr(expr->right, entry, st);
    }
    case FIND_EXPR_COMMA:
        (void)find_eval_expr(expr->left, entry, st);
        if (st->stop && *st->stop)
            return false;
        return find_eval_expr(expr->right, entry, st);
    }

    return false;
}

static void find_walk_cb(const struct walk_entry *entry, void *user) {
    struct find_state *st = user;
    struct find_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;
    if (entry->depth < opts->min_depth)
        return;
    if (opts->max_depth >= 0 && entry->depth > opts->max_depth)
        return;

    (void)find_eval_expr(st->expr, entry, st);
}

int bx_find_main(int argc, char **argv) {
    const char *progname = argv[0] ? argv[0] : "find";
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        find_print_help(progname);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        find_print_version(progname);
        return 0;
    }

    struct find_opts opts = {
        .max_depth = -1,
        .min_depth = 0,
    };

    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
            argi++;
        } else {
            break;
        }
    }

    int expr_index = argi;
    while (expr_index < argc && !token_starts_expression(argv[expr_index]))
        expr_index++;

    int root_count = expr_index - argi;
    char **roots = argv + argi;
    if (root_count == 0) {
        static char *default_root[] = { "." };
        roots = default_root;
        root_count = 1;
    }

    char **expr_argv = calloc((size_t)(argc - expr_index + 1), sizeof(*expr_argv));
    if (!expr_argv) {
        fprintf(stderr, "%s: out of memory\n", progname);
        return 1;
    }

    int expr_argc = 0;
    for (int i = expr_index; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
        } else if (strcmp(arg, "-depth") == 0) {
            opts.depth_first = true;
        } else if (strcmp(arg, "-maxdepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-maxdepth", argv[i], &opts.max_depth)) {
                free(expr_argv);
                return 1;
            }
        } else if (strcmp(arg, "-mindepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-mindepth", argv[i], &opts.min_depth)) {
                free(expr_argv);
                return 1;
            }
        } else {
            expr_argv[expr_argc++] = argv[i];
            if ((strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0 ||
                 strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 ||
                 strcmp(arg, "-iwholename") == 0 || strcmp(arg, "-lname") == 0 ||
                 strcmp(arg, "-ilname") == 0 || strcmp(arg, "-type") == 0 ||
                 strcmp(arg, "-inum") == 0 || strcmp(arg, "-links") == 0 ||
                 strcmp(arg, "-uid") == 0 || strcmp(arg, "-gid") == 0 ||
                 strcmp(arg, "-user") == 0 || strcmp(arg, "-group") == 0 ||
                 strcmp(arg, "-perm") == 0 || strcmp(arg, "-size") == 0 ||
                 strcmp(arg, "-amin") == 0 || strcmp(arg, "-atime") == 0 ||
                 strcmp(arg, "-cmin") == 0 || strcmp(arg, "-ctime") == 0 ||
                 strcmp(arg, "-mmin") == 0 || strcmp(arg, "-mtime") == 0 ||
                 strcmp(arg, "-used") == 0 ||
                 strcmp(arg, "-anewer") == 0 || strcmp(arg, "-cnewer") == 0 ||
                 strcmp(arg, "-newer") == 0 ||
                 strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) && i + 1 < argc) {
                expr_argv[expr_argc++] = argv[++i];
            }
        }
    }

    struct find_parser parser = {
        .progname = progname,
        .argv = expr_argv,
        .argc = expr_argc,
        .opts = &opts,
    };

    struct find_expr *expr = NULL;
    if (expr_argc > 0) {
        expr = find_parse_expr(&parser);
        if (!expr || parser.pos != parser.argc) {
            find_expr_free(expr);
            free(expr_argv);
            return 1;
        }
    } else {
        expr = find_expr_new(FIND_EXPR_TRUE);
    }

    if (!expr) {
        free(expr_argv);
        return 1;
    }

    if (parser.explicit_action == false) {
        struct find_expr *print_expr = find_expr_new(FIND_EXPR_PRINT);
        expr = find_make_binary(FIND_EXPR_AND, expr, print_expr);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(expr_argv);
            return 1;
        }
    }

    if (expr_argc > 0) {
        for (int i = 0; i < expr_argc; i++) {
            if (strcmp(expr_argv[i], "-delete") == 0) {
                opts.depth_first = true;
                break;
            }
        }
    }

    bool stop = false;
    struct find_state st = {
        .progname = progname,
        .opts = &opts,
        .expr = expr,
        .stop = &stop,
        .status = 0,
    };
    if (clock_gettime(CLOCK_REALTIME, &st.now) != 0) {
        st.now.tv_sec = time(NULL);
        st.now.tv_nsec = 0;
    }

    struct walk_opts wopts = {
        .hidden = true,
        .no_ignore = true,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = opts.follow_root_symlink,
        .post_order = opts.depth_first,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    find_expr_free(expr);
    free(expr_argv);
    return st.status;
}
