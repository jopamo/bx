#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "fd_match.h"
#include "lib/checked_math.h"
#include "lib/path_ops.h"

static uint32_t fd_compile_flags(const struct fd_opts *opts,
                                 const char *pattern) {
    if (opts->case_sensitive)
        return 0;
    if (opts->ignore_case)
        return PCRE2_CASELESS;
    if (opts->smart_case && pattern) {
        for (const char *ch = pattern; *ch; ch++) {
            if (*ch >= 'A' && *ch <= 'Z')
                return 0;
        }
        return PCRE2_CASELESS;
    }
    return 0;
}

static pcre2_code *fd_compile_regex(const char *progname, const char *pattern,
                                    const char *display_pattern,
                                    uint32_t flags) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern,
                                   PCRE2_ZERO_TERMINATED, flags, &errcode,
                                   &erroffset, NULL);
    if (re)
        return re;

    PCRE2_UCHAR errbuf[256];
    int msg_rc = pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
    fprintf(stderr,
            "%s: invalid pattern '%s': regex parse error at offset %zu: %s\n",
            progname, display_pattern ? display_pattern : pattern,
            (size_t)erroffset,
            msg_rc >= 0 ? (const char *)errbuf : "regex compile failed");
    return NULL;
}

static pcre2_code *fd_compile_pattern(const char *progname,
                                      const struct fd_opts *opts,
                                      const char *pattern) {
    if (!pattern)
        return NULL;

    if (!opts->glob_match && !opts->fixed_strings) {
        uint32_t flags = fd_compile_flags(opts, pattern);
        return fd_compile_regex(progname, pattern, pattern, flags);
    }

    if (opts->glob_match) {
        size_t translated_len = 0;
        for (const char *ch = pattern; *ch; ch++) {
            size_t add = (*ch == '*' || *ch == '.') ? 2u : 1u;
            if (!bx_checked_size_add(translated_len, add, &translated_len)) {
                fprintf(stderr, "%s: out of memory\n", progname);
                return NULL;
            }
        }
        size_t alloc_len = 0;
        if (!bx_checked_size_add(translated_len, 1u, &alloc_len)) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return NULL;
        }
        char *buf = malloc(alloc_len);
        if (!buf) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return NULL;
        }

        char *p = buf;
        for (const char *ch = pattern; *ch; ch++) {
            switch (*ch) {
            case '*':
                *p++ = '.';
                *p++ = '*';
                break;
            case '?':
                *p++ = '.';
                break;
            case '.':
                *p++ = '\\';
                *p++ = '.';
                break;
            default:
                *p++ = *ch;
                break;
            }
        }
        *p = '\0';
        pcre2_code *re = fd_compile_regex(progname, buf, pattern,
                                          fd_compile_flags(opts, pattern));
        free(buf);
        return re;
    }

    size_t len = strlen(pattern);
    char *buf = malloc(len * 2 + 3);
    if (!buf) {
        fprintf(stderr, "%s: out of memory\n", progname);
        return NULL;
    }

    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        char ch = pattern[i];
        if (ch == '.' || ch == '\\' || ch == '+' || ch == '*' || ch == '?' ||
            ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '{' ||
            ch == '}' || ch == '^' || ch == '$' || ch == '|')
            *p++ = '\\';
        *p++ = ch;
    }
    *p = '\0';

    pcre2_code *re = fd_compile_regex(progname, buf, pattern,
                                      fd_compile_flags(opts, pattern));
    free(buf);
    return re;
}

static bool fd_match_name(const struct fd_state *st, const char *name) {
    if (st->regex_count == 0)
        return true;

    for (int i = 0; i < st->regex_count; i++) {
        int rc = pcre2_match(st->regexes[i], (PCRE2_SPTR)name, strlen(name),
                             0, 0, st->match_data[i], NULL);
        if (rc < 0)
            return false;
    }
    return true;
}

static bool fd_record_match(struct fd_state *st, const char *path,
                            bool is_dir) {
    struct fd_opts *opts = st->opts;
    opts->results++;

    if (opts->exec_mode != FD_EXEC_NONE) {
        if (!fd_exec_items_append_path(&st->exec_items, &st->render, path)) {
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->exec_collect_failed = true;
            if (st->stop)
                *st->stop = true;
            return false;
        }
    } else if (opts->list_details) {
        char *path_copy = strdup(path);
        if (!path_copy) {
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->output_collect_failed = true;
            if (st->stop)
                *st->stop = true;
            return false;
        }

        struct bx_walk_entry entry = {
            .path = path_copy,
            .is_dir = is_dir,
        };
        bool ok =
            fd_detail_items_append(&st->detail_items, &st->render, &entry);
        free(path_copy);
        if (!ok) {
            fprintf(stderr, "%s: out of memory\n", st->progname);
            st->output_collect_failed = true;
            if (st->stop)
                *st->stop = true;
            return false;
        }
    } else if (!opts->quiet) {
        if (!fd_print_match_output(st->writer, &st->render, opts, path, is_dir)) {
            st->output_collect_failed = true;
            if (st->stop)
                *st->stop = true;
            return false;
        }
    }

    if ((opts->max_results > 0 && opts->results >= opts->max_results) ||
        (opts->quiet && opts->results > 0)) {
        if (st->stop)
            *st->stop = true;
    }
    return true;
}

bool fd_state_init(struct fd_state *st, const char *progname,
                   struct fd_opts *opts, bool *stop,
                   bool using_implicit_root,
                   struct bx_line_writer *writer) {
    memset(st, 0, sizeof(*st));
    st->opts = opts;
    st->progname = progname;
    st->stop = stop;
    st->strip_implicit_dot_prefix = using_implicit_root;
    st->writer = writer;

    if (opts->absolute_path)
        st->cwd = bx_path_getcwd_dup();

    fd_render_ctx_init(&st->render, opts, using_implicit_root, st->cwd);

    if (opts->pattern)
        st->regexes[st->regex_count++] =
            fd_compile_pattern(progname, opts, opts->pattern);
    for (int i = 0; i < opts->num_and_patterns; i++)
        st->regexes[st->regex_count++] =
            fd_compile_pattern(progname, opts, opts->and_patterns[i]);

    for (int i = 0; i < st->regex_count; i++) {
        if (!st->regexes[i])
            return false;
        st->match_data[i] =
            pcre2_match_data_create_from_pattern(st->regexes[i], NULL);
        if (!st->match_data[i]) {
            fprintf(stderr, "%s: out of memory\n", progname);
            return false;
        }
    }

    return true;
}

void fd_state_cleanup(struct fd_state *st) {
    if (!st)
        return;

    for (int i = 0; i < st->regex_count; i++) {
        if (st->match_data[i])
            pcre2_match_data_free(st->match_data[i]);
        if (st->regexes[i])
            pcre2_code_free(st->regexes[i]);
    }
    fd_exec_items_free(&st->exec_items);
    fd_detail_items_free(&st->detail_items);
    free(st->cwd);
    memset(st, 0, sizeof(*st));
}

enum bx_walk_action fd_walk_callback(struct bx_walk_entry *entry, void *user) {
    struct fd_state *st = user;
    struct fd_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return BX_WALK_STOP;

    if (opts->max_results > 0 && opts->results >= opts->max_results) {
        if (st->stop)
            *st->stop = true;
        return BX_WALK_STOP;
    }

    if (opts->quiet && opts->results > 0) {
        if (st->stop)
            *st->stop = true;
        return BX_WALK_STOP;
    }

    if (entry->depth == 0 && entry->is_dir)
        return BX_WALK_CONTINUE;

    if (opts->exact_depth >= 0) {
        if (entry->depth != opts->exact_depth)
            return BX_WALK_CONTINUE;
    } else if (entry->depth < opts->min_depth) {
        return BX_WALK_CONTINUE;
    }

    const char *name = opts->full_path ? entry->path : bx_path_basename_ptr(entry->path);

    if (opts->extension) {
        const char *dot = bx_path_extension_ptr(entry->path);
        if (!dot || strcasecmp(dot + 1, opts->extension) != 0)
            return BX_WALK_CONTINUE;
    }

    if (st->regex_count == 0) {
        fd_record_match(st, entry->path, entry->is_dir);
        return (st->stop && *st->stop) ? BX_WALK_STOP : BX_WALK_CONTINUE;
    }

    if (fd_match_name(st, name))
        fd_record_match(st, entry->path, entry->is_dir);
    return (st->stop && *st->stop) ? BX_WALK_STOP : BX_WALK_CONTINUE;
}
