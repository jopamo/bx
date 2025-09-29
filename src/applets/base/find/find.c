#define _GNU_SOURCE
#include <grp.h>
#include <inttypes.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "search/metadata.h"
#include "search/walk.h"

struct find_opts {
    bool depth_first;
    int max_depth;
    int min_depth;
    bool follow_symlinks;
    bool follow_root_symlink;
    bool stay_on_filesystem;
    const char *files0_from;
};

enum find_expr_kind {
    FIND_EXPR_TRUE,
    FIND_EXPR_FALSE,
    FIND_EXPR_NAME,
    FIND_EXPR_PATH,
    FIND_EXPR_REGEX,
    FIND_EXPR_LNAME,
    FIND_EXPR_TYPE,
    FIND_EXPR_XTYPE,
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
    FIND_EXPR_PRINTF,
    FIND_EXPR_LS,
    FIND_EXPR_FPRINTF,
    FIND_EXPR_FLS,
    FIND_EXPR_FPRINT,
    FIND_EXPR_FPRINT0,
    FIND_EXPR_DELETE,
    FIND_EXPR_PRUNE,
    FIND_EXPR_QUIT,
    FIND_EXPR_EXEC,
    FIND_EXPR_OK,
    FIND_EXPR_EXEC_PLUS,
    FIND_EXPR_EXECDIR,
    FIND_EXPR_OKDIR,
    FIND_EXPR_EXECDIR_PLUS,
    FIND_EXPR_NOT,
    FIND_EXPR_AND,
    FIND_EXPR_OR,
    FIND_EXPR_COMMA,
};

struct find_exec_items {
    char **v;
    int count;
    int cap;
};

struct find_root_list {
    char **v;
    int count;
    int cap;
};

struct find_expr {
    enum find_expr_kind kind;
    struct find_expr *left;
    struct find_expr *right;
    const char *text;
    const char *text2;
    char type_filter;
    bool ignore_case;
    long long number;
    int number_cmp;
    mode_t perm_bits;
    int perm_kind;
    unsigned long long size_unit;
    struct timespec ref_time;
    char **exec_argv;
    int exec_argc;
    struct find_exec_items exec_items;
    regex_t regex;
    bool regex_compiled;
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

static volatile sig_atomic_t find_interrupt_signal = 0;

struct find_signal_handlers {
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    bool has_int;
    bool has_term;
    bool has_hup;
};

static void find_handle_interrupt_signal(int signo) {
    find_interrupt_signal = signo;
}

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
    puts("  -files0-from FILE  read starting points from a NUL-delimited FILE");
    puts("  -mount        do not descend directories on other filesystems");
    puts("  -maxdepth N   descend at most N levels below the roots");
    puts("  -mindepth N   do not act on levels less than N");
    puts("  -xdev         same as -mount");
    puts("  -name PATTERN match basename against PATTERN");
    puts("  -lname PATTERN match symlink target against PATTERN");
    puts("  -regex PATTERN match whole path against PATTERN");
    puts("  -iregex PATTERN match whole path against PATTERN, case-insensitively");
    puts("  -regextype TYPE  select regex syntax (currently: posix-extended)");
    puts("  -type [fdl]   match file type");
    puts("  -xtype [fdl]  match the alternate type across symlink dereference");
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
    puts("  -printf FORMAT  write formatted output");
    puts("  -ls           list entry in a GNU find -ls style format");
    puts("  -fprintf FILE FORMAT  write formatted output to FILE");
    puts("  -fls FILE     write -ls style output to FILE");
    puts("  -fprint FILE  write path to FILE");
    puts("  -fprint0 FILE write path followed by NUL to FILE");
    puts("  -delete       delete matched entries");
    puts("  -prune        do not descend into matched directories");
    puts("  -quit         stop after the first deciding result");
    puts("  -exec CMD ... {} ;  run CMD once per matched path");
    puts("  -ok CMD ... {} ;    prompt, then run CMD once per matched path");
    puts("  -exec CMD ... {} +  run CMD with batched matched paths");
    puts("  -execdir CMD ... {} ;  run CMD once per matched path from its parent directory");
    puts("  -okdir CMD ... {} ;  prompt, then run CMD once per matched path from its parent directory");
    puts("  -execdir CMD ... {} +  run CMD with batched matched paths from their parent directory");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

static void find_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static int find_install_one_signal_handler(int signo, struct sigaction *old_action) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = find_handle_interrupt_signal;
    sigemptyset(&sa.sa_mask);
    return sigaction(signo, &sa, old_action);
}

static int find_install_signal_handlers(const char *progname,
                                        struct find_signal_handlers *handlers) {
    memset(handlers, 0, sizeof(*handlers));
    find_interrupt_signal = 0;

    if (find_install_one_signal_handler(SIGINT, &handlers->old_int) != 0) {
        fprintf(stderr, "%s: cannot install SIGINT handler: %s\n", progname, strerror(errno));
        return 1;
    }
    handlers->has_int = true;

    if (find_install_one_signal_handler(SIGTERM, &handlers->old_term) != 0) {
        fprintf(stderr, "%s: cannot install SIGTERM handler: %s\n", progname, strerror(errno));
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_int = false;
        return 1;
    }
    handlers->has_term = true;

    if (find_install_one_signal_handler(SIGHUP, &handlers->old_hup) != 0) {
        fprintf(stderr, "%s: cannot install SIGHUP handler: %s\n", progname, strerror(errno));
        sigaction(SIGTERM, &handlers->old_term, NULL);
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_term = false;
        handlers->has_int = false;
        return 1;
    }
    handlers->has_hup = true;

    return 0;
}

static void find_restore_signal_handlers(struct find_signal_handlers *handlers) {
    if (handlers->has_hup)
        sigaction(SIGHUP, &handlers->old_hup, NULL);
    if (handlers->has_term)
        sigaction(SIGTERM, &handlers->old_term, NULL);
    if (handlers->has_int)
        sigaction(SIGINT, &handlers->old_int, NULL);
}

static int find_finish_interrupted_exec(struct bx_child *child, int *running) {
    int signo = (int)find_interrupt_signal;
    if (signo == 0)
        return 0;

    bx_child_signal_all(child, *running, signo);
    while (*running > 0) {
        if (bx_child_reap(child, running, true, true, NULL, NULL) != 0)
            return 1;
    }
    return 128 + signo;
}

static int find_interrupt_return_code(void) {
    return find_interrupt_signal != 0 ? 128 + (int)find_interrupt_signal : 0;
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

static bool find_root_list_append_copy(struct find_root_list *roots, const char *text, size_t len) {
    if (roots->count >= roots->cap) {
        int new_cap = roots->cap == 0 ? 8 : roots->cap * 2;
        char **tmp = realloc(roots->v, (size_t)new_cap * sizeof(*roots->v));
        if (!tmp)
            return false;
        roots->v = tmp;
        roots->cap = new_cap;
    }

    char *copy = strndup(text, len);
    if (!copy)
        return false;
    roots->v[roots->count++] = copy;
    return true;
}

static void find_root_list_free(struct find_root_list *roots) {
    if (!roots)
        return;
    for (int i = 0; i < roots->count; i++)
        free(roots->v[i]);
    free(roots->v);
    roots->v = NULL;
    roots->count = 0;
    roots->cap = 0;
}

static bool find_load_files0_roots(const char *progname, const char *source,
                                   struct find_root_list *roots) {
    FILE *fp = NULL;
    if (strcmp(source, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(source, "rb");
        if (!fp) {
            find_report_error(progname, source, errno);
            return false;
        }
    }

    char *item = NULL;
    size_t cap = 0;
    ssize_t len = 0;
    bool ok = true;
    while ((len = getdelim(&item, &cap, '\0', fp)) != -1) {
        size_t item_len = (size_t)len;
        if (item_len > 0 && item[item_len - 1] == '\0')
            item_len--;
        if (item_len == 0)
            continue;
        if (!find_root_list_append_copy(roots, item, item_len)) {
            fprintf(stderr, "%s: out of memory\n", progname);
            ok = false;
            break;
        }
    }

    if (ok && ferror(fp)) {
        find_report_error(progname, source, errno ? errno : EIO);
        ok = false;
    }

    free(item);
    if (fp != stdin)
        fclose(fp);
    return ok;
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

static bool find_parse_user_id(const char *progname, const char *text, long long *value) {
    uid_t uid = 0;
    if (bx_walk_resolve_user(text, &uid)) {
        *value = (long long)uid;
        return true;
    }

    fprintf(stderr, "%s: invalid user name or UID argument to -user: %s\n", progname, text);
    return false;
}

static bool find_parse_group_id(const char *progname, const char *text, long long *value) {
    gid_t gid = 0;
    if (bx_walk_resolve_group(text, &gid)) {
        *value = (long long)gid;
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

static bool find_compile_regex(const char *progname, const char *optname,
                               const char *pattern, bool ignore_case,
                               regex_t *out) {
    int flags = REG_EXTENDED;
#ifdef REG_ICASE
    if (ignore_case)
        flags |= REG_ICASE;
#else
    (void)ignore_case;
#endif
    int rc = regcomp(out, pattern, flags);
    if (rc == 0)
        return true;

    char errbuf[256];
    regerror(rc, out, errbuf, sizeof(errbuf));
    fprintf(stderr, "%s: invalid argument to %s: %s (%s)\n",
            progname, optname, pattern ? pattern : "(null)", errbuf);
    return false;
}

static bool find_match_regex(regex_t *regex, const char *text) {
    regmatch_t match;
    if (!regex || !text)
        return false;
    if (regexec(regex, text, 1, &match, 0) != 0)
        return false;
    return match.rm_so == 0 && (size_t)match.rm_eo == strlen(text);
}

static bool find_parse_regextype(const char *progname, const char *text) {
    if (text && strcmp(text, "posix-extended") == 0) {
        return true;
    }

    fprintf(stderr, "%s: unsupported argument to -regextype: %s\n",
            progname, text ? text : "(null)");
    return false;
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
    return bx_walk_numeric_match(age, expected, cmp);
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
    return bx_walk_numeric_match(days, expected, cmp);
}

static const char *find_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool find_match_pattern(const char *pattern, const char *text, bool ignore_case) {
    return fnmatch(pattern, text, ignore_case ? FNM_CASEFOLD : 0) == 0;
}

static bool find_match_link_target(struct walk_entry *entry, const char *pattern, bool ignore_case) {
    if (!walk_entry_load_metadata(entry))
        return false;
    if (!S_ISLNK(entry->mode))
        return false;

    char buf[PATH_MAX + 1];
    ssize_t len = readlink(entry->path, buf, PATH_MAX);
    if (len < 0)
        return false;
    buf[len] = '\0';
    return find_match_pattern(pattern, buf, ignore_case);
}

static bool find_stat_matches_type(const struct stat *st, char type_filter) {
    switch (type_filter) {
    case 'f':
        return S_ISREG(st->st_mode);
    case 'd':
        return S_ISDIR(st->st_mode);
    case 'l':
        return S_ISLNK(st->st_mode);
    case 'p':
        return S_ISFIFO(st->st_mode);
    case 's':
        return S_ISSOCK(st->st_mode);
    case 'b':
        return S_ISBLK(st->st_mode);
    case 'c':
        return S_ISCHR(st->st_mode);
    default:
        return false;
    }
}

static bool find_match_xtype(struct walk_entry *entry, char type_filter) {
    struct stat lst;
    if (lstat(entry->path, &lst) != 0)
        return false;
    if (!S_ISLNK(lst.st_mode))
        return find_stat_matches_type(&lst, type_filter);

    struct stat st;
    if (stat(entry->path, &st) != 0)
        return type_filter == 'l';

    if (entry->follow_metadata)
        return type_filter == 'l';
    return find_stat_matches_type(&st, type_filter);
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

static bool find_write_stream_bytes(FILE *fp, const void *data, size_t len) {
    return len == 0 || fwrite(data, 1, len, fp) == len;
}

static bool find_write_stream_char(FILE *fp, char ch) {
    return fputc((unsigned char)ch, fp) != EOF;
}

static bool find_write_printf_format(FILE *fp, const char *format, const struct walk_entry *entry) {
    if (!format || !entry)
        return false;

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '\\') {
            i++;
            if (format[i] == '\0')
                return find_write_stream_char(fp, '\\');
            switch (format[i]) {
            case '\\':
                if (!find_write_stream_char(fp, '\\'))
                    return false;
                break;
            case '0':
                if (!find_write_stream_char(fp, '\0'))
                    return false;
                break;
            case 'a':
                if (!find_write_stream_char(fp, '\a'))
                    return false;
                break;
            case 'b':
                if (!find_write_stream_char(fp, '\b'))
                    return false;
                break;
            case 'f':
                if (!find_write_stream_char(fp, '\f'))
                    return false;
                break;
            case 'n':
                if (!find_write_stream_char(fp, '\n'))
                    return false;
                break;
            case 'r':
                if (!find_write_stream_char(fp, '\r'))
                    return false;
                break;
            case 't':
                if (!find_write_stream_char(fp, '\t'))
                    return false;
                break;
            case 'v':
                if (!find_write_stream_char(fp, '\v'))
                    return false;
                break;
            case 'c':
                return true;
            default:
                if (!find_write_stream_char(fp, '\\') ||
                    !find_write_stream_char(fp, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (format[i] == '%') {
            i++;
            if (format[i] == '\0')
                return find_write_stream_char(fp, '%');
            switch (format[i]) {
            case '%':
                if (!find_write_stream_char(fp, '%'))
                    return false;
                break;
            case 'p':
                if (!find_write_stream_bytes(fp, entry->path, strlen(entry->path)))
                    return false;
                break;
            default:
                if (!find_write_stream_char(fp, '%') ||
                    !find_write_stream_char(fp, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (!find_write_stream_char(fp, format[i]))
            return false;
    }

    return true;
}

static char find_mode_type_char(mode_t mode) {
    if (S_ISREG(mode))
        return '-';
    if (S_ISDIR(mode))
        return 'd';
    if (S_ISLNK(mode))
        return 'l';
    if (S_ISCHR(mode))
        return 'c';
    if (S_ISBLK(mode))
        return 'b';
    if (S_ISFIFO(mode))
        return 'p';
#ifdef S_ISSOCK
    if (S_ISSOCK(mode))
        return 's';
#endif
    return '?';
}

static void find_mode_to_string(mode_t mode, char out[11]) {
    out[0] = find_mode_type_char(mode);
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';

    if (mode & S_ISUID)
        out[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID)
        out[6] = (mode & S_IXGRP) ? 's' : 'S';
#ifdef S_ISVTX
    if (mode & S_ISVTX)
        out[9] = (mode & S_IXOTH) ? 't' : 'T';
#endif
    out[10] = '\0';
}

static const char *find_user_name(uid_t uid, char numeric_buffer[32]) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && pw->pw_name[0] != '\0')
        return pw->pw_name;
    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

static const char *find_group_name(gid_t gid, char numeric_buffer[32]) {
    struct group *gr = getgrgid(gid);
    if (gr && gr->gr_name && gr->gr_name[0] != '\0')
        return gr->gr_name;
    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}

static void find_format_timestamp(time_t timestamp, char buffer[32]) {
    time_t now = time(NULL);
    if (now == (time_t)-1)
        now = timestamp;

    struct tm tm_value;
    if (!localtime_r(&timestamp, &tm_value)) {
        snprintf(buffer, 32, "??? ?? ??:??");
        return;
    }

    double delta = difftime(now, timestamp);
    if (delta < 0.0)
        delta = -delta;

    const char *fmt = (delta > (365.0 / 2.0) * 24.0 * 60.0 * 60.0 ||
                       timestamp > now + 3600)
        ? "%b %e  %Y"
        : "%b %e %H:%M";
    if (strftime(buffer, 32, fmt, &tm_value) == 0)
        snprintf(buffer, 32, "??? ?? ??:??");
}

static bool find_write_ls_entry(FILE *fp, const struct walk_entry *entry) {
    if (!fp || !entry)
        return false;

    struct stat lst;
    struct stat st;
    bool have_lstat = lstat(entry->path, &lst) == 0;
    bool have_stat = entry->follow_metadata && stat(entry->path, &st) == 0;
    const struct stat *display = NULL;
    if (have_stat)
        display = &st;
    else if (have_lstat)
        display = &lst;
    else
        return false;

    char mode[11];
    char user_numeric[32];
    char group_numeric[32];
    char timestamp[32];
    find_mode_to_string(display->st_mode, mode);
    const char *user_name = find_user_name(display->st_uid, user_numeric);
    const char *group_name = find_group_name(display->st_gid, group_numeric);
    find_format_timestamp(display->st_mtime, timestamp);

    uintmax_t blocks = 0;
    if (display->st_blocks > 0)
        blocks = (uintmax_t)display->st_blocks / 2u;

    if (fprintf(fp, "%10" PRIuMAX " %6" PRIuMAX " %s %3" PRIuMAX " %-8s %-8s ",
                (uintmax_t)display->st_ino, blocks, mode,
                (uintmax_t)display->st_nlink, user_name, group_name) < 0) {
        return false;
    }

    if (S_ISCHR(display->st_mode) || S_ISBLK(display->st_mode)) {
        if (fprintf(fp, "%3" PRIuMAX ", %3" PRIuMAX " %s %s",
                    (uintmax_t)major(display->st_rdev),
                    (uintmax_t)minor(display->st_rdev),
                    timestamp, entry->path) < 0) {
            return false;
        }
    } else if (fprintf(fp, "%8jd %s %s",
                       (intmax_t)display->st_size, timestamp, entry->path) < 0) {
        return false;
    }

    if (have_lstat && S_ISLNK(lst.st_mode) && (!entry->follow_metadata || !have_stat)) {
        char link_target[PATH_MAX + 1];
        ssize_t len = readlink(entry->path, link_target, PATH_MAX);
        if (len >= 0) {
            link_target[len] = '\0';
            if (fprintf(fp, " -> %s", link_target) < 0)
                return false;
        }
    }

    return fputc('\n', fp) != EOF;
}

static struct find_expr *find_expr_new(enum find_expr_kind kind) {
    struct find_expr *expr = calloc(1, sizeof(*expr));
    if (!expr)
        return NULL;
    expr->kind = kind;
    return expr;
}

static bool find_exec_items_append(struct find_exec_items *items, char *text) {
    if (items->count >= items->cap) {
        int new_cap = items->cap == 0 ? 16 : items->cap * 2;
        char **tmp = realloc(items->v, (size_t)new_cap * sizeof(*items->v));
        if (!tmp)
            return false;
        items->v = tmp;
        items->cap = new_cap;
    }

    items->v[items->count++] = text;
    return true;
}

static void find_exec_items_free(struct find_exec_items *items) {
    if (!items)
        return;
    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    items->v = NULL;
    items->count = 0;
    items->cap = 0;
}

static void find_expr_free(struct find_expr *expr) {
    if (!expr)
        return;
    find_expr_free(expr->left);
    find_expr_free(expr->right);
    if (expr->regex_compiled)
        regfree(&expr->regex);
    free(expr->exec_argv);
    find_exec_items_free(&expr->exec_items);
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
static bool find_run_exec_one(struct find_state *st, struct find_expr *expr,
                              const char *path, const char *cwd);
static bool find_execdir_split_path(const char *path, char **dir_out, char **arg_out);

static bool find_prompt_ok(const char *cmdname, const char *path) {
    fprintf(stderr, "< %s ... %s > ? ", cmdname, path);
    fflush(stderr);

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, stdin);
    if (len < 0) {
        free(line);
        return false;
    }

    bool approved = len > 0 && (line[0] == 'y' || line[0] == 'Y');
    free(line);
    return approved;
}

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
    } else if (strcmp(arg, "-regex") == 0 || strcmp(arg, "-iregex") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_REGEX);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iregex") == 0;
            if (!find_compile_regex(parser->progname, arg, expr->text,
                                    expr->ignore_case, &expr->regex)) {
                find_expr_free(expr);
                return NULL;
            }
            expr->regex_compiled = true;
        }
    } else if (strcmp(arg, "-regextype") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-regextype'\n", parser->progname);
            return NULL;
        }
        if (!find_parse_regextype(parser->progname, parser->argv[parser->pos])) {
            return NULL;
        }
        parser->pos++;
        expr = find_expr_new(FIND_EXPR_TRUE);
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
            !bx_walk_type_filter_is_valid(type_arg[0], false)) {
            fprintf(stderr, "%s: unknown argument to -type: %s\n", parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_TYPE);
        if (expr)
            expr->type_filter = type_arg[0];
    } else if (strcmp(arg, "-xtype") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-xtype'\n", parser->progname);
            return NULL;
        }
        const char *type_arg = parser->argv[parser->pos++];
        if (type_arg[0] == '\0' || type_arg[1] != '\0' ||
            !bx_walk_type_filter_is_valid(type_arg[0], false)) {
            fprintf(stderr, "%s: unknown argument to -xtype: %s\n", parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_XTYPE);
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
    } else if (strcmp(arg, "-printf") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-printf'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_PRINTF);
        if (expr)
            expr->text = parser->argv[parser->pos++];
    } else if (strcmp(arg, "-ls") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_LS);
    } else if (strcmp(arg, "-fprintf") == 0) {
        if (parser->pos + 1 >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-fprintf'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_FPRINTF);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->text2 = parser->argv[parser->pos++];
        }
    } else if (strcmp(arg, "-fls") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-fls'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_FLS);
        if (expr)
            expr->text = parser->argv[parser->pos++];
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
    } else if (strcmp(arg, "-prune") == 0) {
        expr = find_expr_new(FIND_EXPR_PRUNE);
    } else if (strcmp(arg, "-quit") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_QUIT);
    } else if (strcmp(arg, "-exec") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-exec'\n", parser->progname);
            return NULL;
        }
        int command_start = parser->pos;
        int command_end = -1;
        bool per_item = false;
        bool saw_placeholder = false;
        for (int i = parser->pos; i < parser->argc; i++) {
            if (strcmp(parser->argv[i], ";") == 0) {
                command_end = i;
                per_item = true;
                break;
            }
            if (strcmp(parser->argv[i], "+") == 0) {
                command_end = i;
                break;
            }
            if (strcmp(parser->argv[i], "{}") == 0)
                saw_placeholder = true;
        }
        if (command_end < 0) {
            fprintf(stderr, "%s: missing terminating `;' or `+' for `-exec'\n",
                    parser->progname);
            return NULL;
        }
        if (!saw_placeholder) {
            fprintf(stderr, "%s: missing '{}' in `-exec'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(per_item ? FIND_EXPR_EXEC : FIND_EXPR_EXEC_PLUS);
        if (expr) {
            expr->exec_argc = command_end - command_start;
            expr->exec_argv = calloc((size_t)expr->exec_argc + 1, sizeof(*expr->exec_argv));
            if (!expr->exec_argv) {
                find_expr_free(expr);
                fprintf(stderr, "%s: out of memory\n", parser->progname);
                return NULL;
            }
            for (int i = 0; i < expr->exec_argc; i++)
                expr->exec_argv[i] = parser->argv[command_start + i];
            expr->exec_argv[expr->exec_argc] = NULL;
        }
        parser->pos = command_end + 1;
    } else if (strcmp(arg, "-ok") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-ok'\n", parser->progname);
            return NULL;
        }
        int command_start = parser->pos;
        int command_end = -1;
        bool saw_placeholder = false;
        for (int i = parser->pos; i < parser->argc; i++) {
            if (strcmp(parser->argv[i], ";") == 0) {
                command_end = i;
                break;
            }
            if (strcmp(parser->argv[i], "{}") == 0)
                saw_placeholder = true;
        }
        if (command_end < 0) {
            fprintf(stderr, "%s: missing terminating `;' for `-ok'\n", parser->progname);
            return NULL;
        }
        if (!saw_placeholder) {
            fprintf(stderr, "%s: missing '{}' in `-ok'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_OK);
        if (expr) {
            expr->exec_argc = command_end - command_start;
            expr->exec_argv = calloc((size_t)expr->exec_argc + 1, sizeof(*expr->exec_argv));
            if (!expr->exec_argv) {
                find_expr_free(expr);
                fprintf(stderr, "%s: out of memory\n", parser->progname);
                return NULL;
            }
            for (int i = 0; i < expr->exec_argc; i++)
                expr->exec_argv[i] = parser->argv[command_start + i];
            expr->exec_argv[expr->exec_argc] = NULL;
        }
        parser->pos = command_end + 1;
    } else if (strcmp(arg, "-okdir") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-okdir'\n", parser->progname);
            return NULL;
        }
        int command_start = parser->pos;
        int command_end = -1;
        bool saw_placeholder = false;
        for (int i = parser->pos; i < parser->argc; i++) {
            if (strcmp(parser->argv[i], ";") == 0) {
                command_end = i;
                break;
            }
            if (strcmp(parser->argv[i], "{}") == 0)
                saw_placeholder = true;
        }
        if (command_end < 0) {
            fprintf(stderr, "%s: missing terminating `;' for `-okdir'\n", parser->progname);
            return NULL;
        }
        if (!saw_placeholder) {
            fprintf(stderr, "%s: missing '{}' in `-okdir'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_OKDIR);
        if (expr) {
            expr->exec_argc = command_end - command_start;
            expr->exec_argv = calloc((size_t)expr->exec_argc + 1, sizeof(*expr->exec_argv));
            if (!expr->exec_argv) {
                find_expr_free(expr);
                fprintf(stderr, "%s: out of memory\n", parser->progname);
                return NULL;
            }
            for (int i = 0; i < expr->exec_argc; i++)
                expr->exec_argv[i] = parser->argv[command_start + i];
            expr->exec_argv[expr->exec_argc] = NULL;
        }
        parser->pos = command_end + 1;
    } else if (strcmp(arg, "-execdir") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-execdir'\n", parser->progname);
            return NULL;
        }
        int command_start = parser->pos;
        int command_end = -1;
        bool per_item = false;
        bool saw_placeholder = false;
        for (int i = parser->pos; i < parser->argc; i++) {
            if (strcmp(parser->argv[i], ";") == 0) {
                command_end = i;
                per_item = true;
                break;
            }
            if (strcmp(parser->argv[i], "+") == 0) {
                command_end = i;
                break;
            }
            if (strcmp(parser->argv[i], "{}") == 0)
                saw_placeholder = true;
        }
        if (command_end < 0) {
            fprintf(stderr, "%s: missing terminating `;' or `+' for `-execdir'\n",
                    parser->progname);
            return NULL;
        }
        if (!saw_placeholder) {
            fprintf(stderr, "%s: missing '{}' in `-execdir'\n", parser->progname);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(per_item ? FIND_EXPR_EXECDIR : FIND_EXPR_EXECDIR_PLUS);
        if (expr) {
            expr->exec_argc = command_end - command_start;
            expr->exec_argv = calloc((size_t)expr->exec_argc + 1, sizeof(*expr->exec_argv));
            if (!expr->exec_argv) {
                find_expr_free(expr);
                fprintf(stderr, "%s: out of memory\n", parser->progname);
                return NULL;
            }
            for (int i = 0; i < expr->exec_argc; i++)
                expr->exec_argv[i] = parser->argv[command_start + i];
            expr->exec_argv[expr->exec_argc] = NULL;
        }
        parser->pos = command_end + 1;
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

static bool find_eval_expr(struct find_expr *expr, struct walk_entry *entry,
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
    case FIND_EXPR_REGEX:
        return find_match_regex(&expr->regex, entry->path);
    case FIND_EXPR_PATH:
        return find_match_pattern(expr->text, entry->path, expr->ignore_case);
    case FIND_EXPR_LNAME:
        return find_match_link_target(entry, expr->text, expr->ignore_case);
    case FIND_EXPR_TYPE:
        return bx_walk_entry_matches_type(entry, expr->type_filter);
    case FIND_EXPR_XTYPE:
        return find_match_xtype(entry, expr->type_filter);
    case FIND_EXPR_INUM:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->inode, expr->number, expr->number_cmp);
    case FIND_EXPR_LINKS:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->nlink, expr->number, expr->number_cmp);
    case FIND_EXPR_UID:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->uid, expr->number, expr->number_cmp);
    case FIND_EXPR_GID:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->gid, expr->number, expr->number_cmp);
    case FIND_EXPR_USER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->uid, expr->number, 0);
    case FIND_EXPR_GROUP:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->gid, expr->number, 0);
    case FIND_EXPR_NOUSER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return !bx_walk_uid_has_passwd(entry->uid);
    case FIND_EXPR_NOGROUP:
        if (!walk_entry_load_metadata(entry))
            return false;
        return !bx_walk_gid_has_group(entry->gid);
    case FIND_EXPR_PERM:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_mode_matches_perm(entry->mode, expr->perm_bits, expr->perm_kind);
    case FIND_EXPR_SIZE:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_size_matches(entry->size, expr->number, expr->number_cmp, expr->size_unit);
    case FIND_EXPR_AMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->atime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_ATIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->atime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_CMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->ctime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_CTIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->ctime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_MMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->mtime, expr->number, expr->number_cmp, 60ULL);
    case FIND_EXPR_MTIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->mtime, expr->number, expr->number_cmp, 86400ULL);
    case FIND_EXPR_USED:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_used_match(entry->atime, entry->ctime, expr->number, expr->number_cmp);
    case FIND_EXPR_ANEWER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_timespec_cmp(entry->atime, expr->ref_time) > 0;
    case FIND_EXPR_CNEWER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_timespec_cmp(entry->ctime, expr->ref_time) > 0;
    case FIND_EXPR_NEWER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_timespec_cmp(entry->mtime, expr->ref_time) > 0;
    case FIND_EXPR_EMPTY:
        return bx_walk_entry_is_empty(entry);
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
    case FIND_EXPR_PRINTF:
        if (!find_write_printf_format(stdout, expr->text, entry)) {
            find_report_error(st->progname, "stdout", errno ? errno : EIO);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_LS:
        if (!find_write_ls_entry(stdout, entry)) {
            find_report_error(st->progname, entry->path, errno ? errno : EIO);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_FPRINTF: {
        FILE *fp = fopen(expr->text, "ab");
        if (!fp) {
            find_report_error(st->progname, expr->text, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        bool ok = find_write_printf_format(fp, expr->text2, entry);
        if (!ok)
            find_report_error(st->progname, expr->text, errno ? errno : EIO);
        fclose(fp);
        if (!ok) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    }
    case FIND_EXPR_FLS: {
        FILE *fp = fopen(expr->text, "ab");
        if (!fp) {
            find_report_error(st->progname, expr->text, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        bool ok = find_write_ls_entry(fp, entry);
        if (!ok)
            find_report_error(st->progname, expr->text, errno ? errno : EIO);
        fclose(fp);
        if (!ok) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    }
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
    case FIND_EXPR_PRUNE:
        if (entry->is_dir)
            entry->prune = true;
        return true;
    case FIND_EXPR_QUIT:
        if (st->stop)
            *st->stop = true;
        return true;
    case FIND_EXPR_EXEC:
        return find_run_exec_one(st, expr, entry->path, NULL);
    case FIND_EXPR_OK:
        if (!find_prompt_ok(expr->exec_argv[0], entry->path))
            return true;
        return find_run_exec_one(st, expr, entry->path, NULL);
    case FIND_EXPR_EXEC_PLUS: {
        char *path = strdup(entry->path);
        if (!path || !find_exec_items_append(&expr->exec_items, path)) {
            free(path);
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    }
    case FIND_EXPR_EXECDIR: {
        char *cwd = NULL;
        char *arg = NULL;
        if (!find_execdir_split_path(entry->path, &cwd, &arg)) {
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            free(cwd);
            free(arg);
            return false;
        }
        bool ok = find_run_exec_one(st, expr, arg, cwd);
        free(cwd);
        free(arg);
        return ok;
    }
    case FIND_EXPR_OKDIR: {
        char *cwd = NULL;
        char *arg = NULL;
        if (!find_execdir_split_path(entry->path, &cwd, &arg)) {
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            free(cwd);
            free(arg);
            return false;
        }
        if (!find_prompt_ok(expr->exec_argv[0], entry->path)) {
            free(cwd);
            free(arg);
            return true;
        }
        bool ok = find_run_exec_one(st, expr, arg, cwd);
        free(cwd);
        free(arg);
        return ok;
    }
    case FIND_EXPR_EXECDIR_PLUS: {
        char *path = strdup(entry->path);
        if (!path || !find_exec_items_append(&expr->exec_items, path)) {
            free(path);
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    }
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

static size_t find_exec_placeholder_count(const char *arg, void *user) {
    (void)user;
    return (arg && strcmp(arg, "{}") == 0) ? 1u : 0u;
}

static size_t find_exec_expanded_bytes(const char *arg, const char *item, void *user) {
    (void)user;
    return strlen((arg && strcmp(arg, "{}") == 0) ? item : arg) + 1;
}

static char *find_exec_expand_arg(const char *arg, const char *item, void *user) {
    (void)user;
    return strdup((arg && strcmp(arg, "{}") == 0) ? item : arg);
}

struct find_exec_batch_ctx {
    struct find_expr *expr;
};

static size_t find_exec_batch_bytes(void *user, int start, int count) {
    struct find_exec_batch_ctx *ctx = user;
    return bx_argv_bytes_with_item_expansion((const char *const *)ctx->expr->exec_argv,
                                             ctx->expr->exec_argc,
                                             ctx->expr->exec_items.v, start, count, 1,
                                             find_exec_placeholder_count,
                                             find_exec_expanded_bytes,
                                             NULL, NULL);
}

static int find_select_exec_batch_count(struct find_expr *expr, int start, size_t char_limit) {
    struct find_exec_batch_ctx ctx = { .expr = expr };
    return bx_argv_select_batch_count_by_bytes(expr->exec_items.count, start, 0, 0,
                                               char_limit, find_exec_batch_bytes, &ctx);
}

static bool find_execdir_split_path(const char *path, char **dir_out, char **arg_out) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t dir_len = slash ? (size_t)(slash - path) : 0;

    char *dir = NULL;
    if (dir_len == 0) {
        dir = strdup(".");
    } else {
        dir = strndup(path, dir_len);
    }
    if (!dir)
        return false;

    size_t arg_len = strlen(base) + 3;
    char *arg = malloc(arg_len);
    if (!arg) {
        free(dir);
        return false;
    }
    snprintf(arg, arg_len, "./%s", base);

    *dir_out = dir;
    *arg_out = arg;
    return true;
}

static void find_execdir_free_split_items(char **items, int count) {
    if (!items)
        return;
    for (int i = 0; i < count; i++)
        free(items[i]);
    free(items);
}

static char **find_execdir_collect_group(struct find_expr *expr, int start,
                                         char **dir_out, int *group_count_out) {
    char *dir = NULL;
    char *first_arg = NULL;
    if (!find_execdir_split_path(expr->exec_items.v[start], &dir, &first_arg))
        return NULL;

    int group_count = 1;
    while (start + group_count < expr->exec_items.count) {
        char *next_dir = NULL;
        char *next_arg = NULL;
        if (!find_execdir_split_path(expr->exec_items.v[start + group_count], &next_dir, &next_arg)) {
            free(dir);
            free(first_arg);
            return NULL;
        }
        bool same_dir = strcmp(dir, next_dir) == 0;
        free(next_dir);
        free(next_arg);
        if (!same_dir)
            break;
        group_count++;
    }

    char **items = calloc((size_t)group_count, sizeof(*items));
    if (!items) {
        free(dir);
        free(first_arg);
        return NULL;
    }
    items[0] = first_arg;
    for (int i = 1; i < group_count; i++) {
        char *item_dir = NULL;
        if (!find_execdir_split_path(expr->exec_items.v[start + i], &item_dir, &items[i])) {
            free(item_dir);
            find_execdir_free_split_items(items, i);
            free(dir);
            return NULL;
        }
        free(item_dir);
    }

    *dir_out = dir;
    *group_count_out = group_count;
    return items;
}

struct find_exec_reap_ctx {
    const char *progname;
    const char *cmdname;
    int *status;
};

static void find_exec_reap_status_cb(pid_t pid, int wait_status, bool exec_failed, int exec_errno,
                                     void *user) {
    (void)pid;
    struct find_exec_reap_ctx *ctx = user;
    if (exec_failed) {
        fprintf(stderr, "%s: failed to run command '%s': %s\n",
                ctx->progname, ctx->cmdname, strerror(exec_errno));
        *ctx->status = 1;
        return;
    }

    if ((WIFEXITED(wait_status) && WEXITSTATUS(wait_status) != 0) || WIFSIGNALED(wait_status))
        *ctx->status = 1;
}

static bool find_run_exec_one(struct find_state *st, struct find_expr *expr,
                              const char *path, const char *cwd) {
    size_t char_limit = bx_argv_effective_char_limit(0);
    struct bx_child child = {0};
    int running = 0;
    int status = 0;
    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();
    struct find_signal_handlers handlers;
    runner_opts.cwd = cwd;

    char *item = strdup(path);
    if (!item) {
        fprintf(stderr, "%s: out of memory\n", st->progname);
        st->status = 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }

    char *items[] = { item };
    char **argv = bx_argv_build_with_item_expansion((const char *const *)expr->exec_argv,
                                                    expr->exec_argc,
                                                    items, 0, 1, 0,
                                                    find_exec_placeholder_count,
                                                    find_exec_expand_arg,
                                                    NULL, NULL);
    free(item);
    if (!argv) {
        fprintf(stderr, "%s: out of memory\n", st->progname);
        st->status = 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }

    if (char_limit > 0 && bx_argv_bytes(argv) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", st->progname);
        bx_argv_free(argv);
        st->status = 1;
        return false;
    }

    if (find_install_signal_handlers(st->progname, &handlers) != 0) {
        bx_argv_free(argv);
        st->status = 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }

    bool exec_failed_now = false;
    int exec_errno_now = 0;
    int spawn_rc = bx_child_spawn_argv(st->progname, argv, &runner_opts, 0,
                                       &child, &running,
                                       &exec_failed_now, &exec_errno_now);
    bx_argv_free(argv);
    if (spawn_rc != 0) {
        if (find_interrupt_signal != 0) {
            int rc = find_finish_interrupted_exec(&child, &running);
            find_restore_signal_handlers(&handlers);
            st->status = rc != 0 ? rc : 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        find_restore_signal_handlers(&handlers);
        st->status = 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }

    struct find_exec_reap_ctx ctx = {
        .progname = st->progname,
        .cmdname = expr->exec_argv[0],
        .status = &status,
    };
    if (bx_child_reap(&child, &running, true, true, find_exec_reap_status_cb, &ctx) != 0) {
        find_restore_signal_handlers(&handlers);
        st->status = 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }
    if (find_interrupt_signal != 0) {
        int rc = find_finish_interrupted_exec(&child, &running);
        find_restore_signal_handlers(&handlers);
        st->status = rc != 0 ? rc : 1;
        if (st->stop)
            *st->stop = true;
        return false;
    }
    find_restore_signal_handlers(&handlers);

    if (status != 0)
        st->status = 1;
    return status == 0;
}

static int find_run_exec_batches(const char *progname, struct find_expr *expr) {
    if (!expr || expr->kind != FIND_EXPR_EXEC_PLUS || expr->exec_items.count == 0)
        return 0;

    size_t char_limit = bx_argv_effective_char_limit(0);
    struct bx_child child = {0};
    int running = 0;
    int status = 0;
    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();

    for (int i = 0; i < expr->exec_items.count; ) {
        int take = find_select_exec_batch_count(expr, i, char_limit);
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            return 1;
        }

        char **argv = bx_argv_build_with_item_expansion((const char *const *)expr->exec_argv,
                                                        expr->exec_argc,
                                                        expr->exec_items.v, i, take, 1,
                                                        find_exec_placeholder_count,
                                                        find_exec_expand_arg,
                                                        NULL, NULL);
        if (!argv) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return 1;
        }

        struct find_signal_handlers handlers;
        if (find_install_signal_handlers(progname, &handlers) != 0) {
            bx_argv_free(argv);
            return 1;
        }

        bool exec_failed_now = false;
        int exec_errno_now = 0;
        int spawn_rc = bx_child_spawn_argv(progname, argv, &runner_opts, 0,
                                           &child, &running,
                                           &exec_failed_now, &exec_errno_now);
        bx_argv_free(argv);
        if (spawn_rc != 0) {
            if (find_interrupt_signal != 0) {
                int rc = find_finish_interrupted_exec(&child, &running);
                find_restore_signal_handlers(&handlers);
                return rc != 0 ? rc : 1;
            }
            find_restore_signal_handlers(&handlers);
            return 1;
        }

        struct find_exec_reap_ctx ctx = {
            .progname = progname,
            .cmdname = expr->exec_argv[0],
            .status = &status,
        };
        if (bx_child_reap(&child, &running, true, true, find_exec_reap_status_cb, &ctx) != 0) {
            find_restore_signal_handlers(&handlers);
            return 1;
        }
        if (find_interrupt_signal != 0) {
            int rc = find_finish_interrupted_exec(&child, &running);
            find_restore_signal_handlers(&handlers);
            return rc != 0 ? rc : 1;
        }
        find_restore_signal_handlers(&handlers);
        i += take;
    }

    return status;
}

static int find_run_execdir_batches(const char *progname, struct find_expr *expr) {
    if (!expr || expr->kind != FIND_EXPR_EXECDIR_PLUS || expr->exec_items.count == 0)
        return 0;

    size_t char_limit = bx_argv_effective_char_limit(0);
    struct bx_child child = {0};
    int running = 0;
    int status = 0;

    for (int i = 0; i < expr->exec_items.count; ) {
        char *cwd = NULL;
        int group_count = 0;
        char **group_items = find_execdir_collect_group(expr, i, &cwd, &group_count);
        if (!group_items || !cwd) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        int take = bx_argv_select_batch_count((const char *const *)expr->exec_argv, expr->exec_argc,
                                              group_items, NULL, group_count, 0, 0, 0, char_limit);
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        char **argv = bx_argv_build_with_item_expansion((const char *const *)expr->exec_argv,
                                                        expr->exec_argc,
                                                        group_items, 0, take, 1,
                                                        find_exec_placeholder_count,
                                                        find_exec_expand_arg,
                                                        NULL, NULL);
        if (!argv) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();
        runner_opts.cwd = cwd;

        struct find_signal_handlers handlers;
        if (find_install_signal_handlers(progname, &handlers) != 0) {
            bx_argv_free(argv);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        bool exec_failed_now = false;
        int exec_errno_now = 0;
        int spawn_rc = bx_child_spawn_argv(progname, argv, &runner_opts, 0,
                                           &child, &running,
                                           &exec_failed_now, &exec_errno_now);
        bx_argv_free(argv);
        free(cwd);
        find_execdir_free_split_items(group_items, group_count);
        if (spawn_rc != 0) {
            if (find_interrupt_signal != 0) {
                int rc = find_finish_interrupted_exec(&child, &running);
                find_restore_signal_handlers(&handlers);
                return rc != 0 ? rc : 1;
            }
            find_restore_signal_handlers(&handlers);
            return 1;
        }

        struct find_exec_reap_ctx ctx = {
            .progname = progname,
            .cmdname = expr->exec_argv[0],
            .status = &status,
        };
        if (bx_child_reap(&child, &running, true, true, find_exec_reap_status_cb, &ctx) != 0) {
            find_restore_signal_handlers(&handlers);
            return 1;
        }
        if (find_interrupt_signal != 0) {
            int rc = find_finish_interrupted_exec(&child, &running);
            find_restore_signal_handlers(&handlers);
            return rc != 0 ? rc : 1;
        }
        find_restore_signal_handlers(&handlers);

        i += take;
    }

    return status;
}

static int find_run_pending_exec_exprs(const char *progname, struct find_expr *expr) {
    if (!expr)
        return 0;

    int status = 0;
    int rc = find_run_pending_exec_exprs(progname, expr->left);
    if (rc > 1)
        return rc;
    if (rc != 0)
        status = 1;
    if (find_interrupt_signal != 0)
        return find_interrupt_return_code();

    rc = find_run_pending_exec_exprs(progname, expr->right);
    if (rc > 1)
        return rc;
    if (rc != 0)
        status = 1;
    if (find_interrupt_signal != 0)
        return find_interrupt_return_code();

    rc = find_run_exec_batches(progname, expr);
    if (rc > 1)
        return rc;
    if (rc != 0)
        status = 1;
    if (find_interrupt_signal != 0)
        return find_interrupt_return_code();

    rc = find_run_execdir_batches(progname, expr);
    if (rc > 1)
        return rc;
    if (rc != 0)
        status = 1;
    return status;
}

static void find_walk_cb(struct walk_entry *entry, void *user) {
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

    int explicit_root_count = expr_index - argi;
    char **explicit_roots = argv + argi;

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
        } else if (strcmp(arg, "-files0-from") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-files0-from'\n", progname);
                free(expr_argv);
                return 1;
            }
            opts.files0_from = argv[i];
        } else if (strcmp(arg, "-mount") == 0 || strcmp(arg, "-xdev") == 0) {
            opts.stay_on_filesystem = true;
        } else {
            expr_argv[expr_argc++] = argv[i];
            if ((strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0 ||
                 strcmp(arg, "-regex") == 0 || strcmp(arg, "-iregex") == 0 ||
                 strcmp(arg, "-regextype") == 0 ||
                 strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 ||
                 strcmp(arg, "-iwholename") == 0 || strcmp(arg, "-lname") == 0 ||
                 strcmp(arg, "-ilname") == 0 || strcmp(arg, "-type") == 0 ||
                 strcmp(arg, "-xtype") == 0 ||
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
                 strcmp(arg, "-fprintf") == 0 ||
                 strcmp(arg, "-fls") == 0 ||
                 strcmp(arg, "-printf") == 0 ||
                 strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) && i + 1 < argc) {
                expr_argv[expr_argc++] = argv[++i];
                if (strcmp(arg, "-fprintf") == 0 && i + 1 < argc)
                    expr_argv[expr_argc++] = argv[++i];
            }
        }
    }

    struct find_root_list root_list = {0};
    char **roots = explicit_roots;
    int root_count = explicit_root_count;
    if (opts.files0_from) {
        if (explicit_root_count > 0) {
            fprintf(stderr, "%s: extra operand `%s'\n", progname, explicit_roots[0]);
            fprintf(stderr, "%s: file operands cannot be combined with -files0-from\n", progname);
            free(expr_argv);
            return 1;
        }
        if (!find_load_files0_roots(progname, opts.files0_from, &root_list)) {
            free(expr_argv);
            find_root_list_free(&root_list);
            return 1;
        }
        roots = root_list.v;
        root_count = root_list.count;
    } else if (root_count == 0) {
        static char *default_root[] = { "." };
        roots = default_root;
        root_count = 1;
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
            find_root_list_free(&root_list);
            return 1;
        }
    } else {
        expr = find_expr_new(FIND_EXPR_TRUE);
    }

    if (!expr) {
        free(expr_argv);
        find_root_list_free(&root_list);
        return 1;
    }

    if (parser.explicit_action == false) {
        struct find_expr *print_expr = find_expr_new(FIND_EXPR_PRINT);
        expr = find_make_binary(FIND_EXPR_AND, expr, print_expr);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(expr_argv);
            find_root_list_free(&root_list);
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
        .stay_on_filesystem = opts.stay_on_filesystem,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
        .cycle_mode = opts.follow_symlinks ? WALK_CYCLE_DIR_REPEAT : WALK_CYCLE_NONE,
        .cycle_report = opts.follow_symlinks ? WALK_CYCLE_ERROR : WALK_CYCLE_IGNORE,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    if (find_interrupt_signal != 0 && st.status == 0)
        st.status = find_interrupt_return_code();

    if (find_interrupt_signal == 0) {
        int pending_rc = find_run_pending_exec_exprs(progname, expr);
        if (pending_rc > 1)
            st.status = pending_rc;
        else if (pending_rc != 0 && st.status == 0)
            st.status = 1;
        else if (pending_rc != 0)
            st.status = 1;
    }

    find_expr_free(expr);
    free(expr_argv);
    find_root_list_free(&root_list);
    return st.status;
}
