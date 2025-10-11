#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "find_exec.h"
#include "find_internal.h"
#include "find_output.h"
#include "lib/path_ops.h"
#include "search/metadata.h"

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

static bool find_match_pattern(const char *pattern, const char *text,
                               bool ignore_case) {
#ifdef FNM_CASEFOLD
    return fnmatch(pattern, text, ignore_case ? FNM_CASEFOLD : 0) == 0;
#else
    if (!ignore_case)
        return fnmatch(pattern, text, 0) == 0;

    size_t pattern_len = strlen(pattern);
    size_t text_len = strlen(text);
    char *lower_pattern = malloc(pattern_len + 1);
    char *lower_text = malloc(text_len + 1);
    if (!lower_pattern || !lower_text) {
        free(lower_pattern);
        free(lower_text);
        return false;
    }

    for (size_t i = 0; i < pattern_len; i++)
        lower_pattern[i] = (char)tolower((unsigned char)pattern[i]);
    lower_pattern[pattern_len] = '\0';
    for (size_t i = 0; i < text_len; i++)
        lower_text[i] = (char)tolower((unsigned char)text[i]);
    lower_text[text_len] = '\0';

    bool matched = fnmatch(lower_pattern, lower_text, 0) == 0;
    free(lower_pattern);
    free(lower_text);
    return matched;
#endif
}

static bool find_match_link_target(struct walk_entry *entry, const char *pattern,
                                   bool ignore_case) {
    if (!walk_entry_load_metadata(entry))
        return false;
    if (!S_ISLNK(entry->mode))
        return false;

    char *target = bx_path_readlink_dup(entry->path);
    if (!target)
        return false;
    bool matched = find_match_pattern(pattern, target, ignore_case);
    free(target);
    return matched;
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

bool find_eval_expr(struct find_expr *expr, struct walk_entry *entry,
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
        return find_match_pattern(expr->text, bx_path_basename_ptr(entry->path),
                                  expr->ignore_case);
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
        return bx_walk_numeric_match((unsigned long long)entry->inode,
                                     expr->number, expr->number_cmp);
    case FIND_EXPR_LINKS:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->nlink,
                                     expr->number, expr->number_cmp);
    case FIND_EXPR_UID:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->uid,
                                     expr->number, expr->number_cmp);
    case FIND_EXPR_GID:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->gid,
                                     expr->number, expr->number_cmp);
    case FIND_EXPR_USER:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->uid,
                                     expr->number, 0);
    case FIND_EXPR_GROUP:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_numeric_match((unsigned long long)entry->gid,
                                     expr->number, 0);
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
        return bx_walk_mode_matches_perm(entry->mode, expr->perm_bits,
                                         expr->perm_kind);
    case FIND_EXPR_SIZE:
        if (!walk_entry_load_metadata(entry))
            return false;
        return bx_walk_size_matches(entry->size, expr->number,
                                    expr->number_cmp, expr->size_unit);
    case FIND_EXPR_AMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->atime, expr->number,
                                   expr->number_cmp, 60ULL);
    case FIND_EXPR_ATIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->atime, expr->number,
                                   expr->number_cmp, 86400ULL);
    case FIND_EXPR_CMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->ctime, expr->number,
                                   expr->number_cmp, 60ULL);
    case FIND_EXPR_CTIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->ctime, expr->number,
                                   expr->number_cmp, 86400ULL);
    case FIND_EXPR_MMIN:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->mtime, expr->number,
                                   expr->number_cmp, 60ULL);
    case FIND_EXPR_MTIME:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_time_age_match(st->now, entry->mtime, expr->number,
                                   expr->number_cmp, 86400ULL);
    case FIND_EXPR_USED:
        if (!walk_entry_load_metadata(entry))
            return false;
        return find_used_match(entry->atime, entry->ctime, expr->number,
                               expr->number_cmp);
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
            find_report_error(st->progname, entry->path,
                              errno ? errno : EIO);
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
