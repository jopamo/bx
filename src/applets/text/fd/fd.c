#include <errno.h>
#define _GNU_SOURCE
#include <fnmatch.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "search/metadata.h"
#include "search/walk.h"
#include "search/options.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#define FD_MAX_AND_PATTERNS 16
#define FD_PLACEHOLDER "{}"

enum fd_exec_mode {
    FD_EXEC_NONE = 0,
    FD_EXEC_EACH,
    FD_EXEC_BATCH,
};

enum fd_strip_cwd_prefix_mode {
    FD_STRIP_CWD_PREFIX_UNSET = 0,
    FD_STRIP_CWD_PREFIX_AUTO,
    FD_STRIP_CWD_PREFIX_ALWAYS,
    FD_STRIP_CWD_PREFIX_NEVER,
};

enum fd_placeholder_kind {
    FD_PH_NONE = 0,
    FD_PH_PATH,
    FD_PH_BASENAME,
    FD_PH_DIRNAME,
    FD_PH_PATH_STEM,
    FD_PH_BASENAME_STEM,
};

struct fd_exec_items {
    char **v;
    int count;
    int cap;
};

struct fd_opts {
    bool hidden;
    bool no_ignore;
    bool follow_symlinks;
    bool absolute_path;
    bool full_path;
    bool ignore_case;
    bool smart_case;
    bool case_sensitive;
    bool fixed_strings;
    bool glob_match;
    bool print0;
    bool quiet;
    bool show_errors;
    bool show_type;
    const char *path_separator;
    enum fd_strip_cwd_prefix_mode strip_cwd_prefix;
    const char *pattern;
    const char *and_patterns[FD_MAX_AND_PATTERNS];
    int num_and_patterns;
    const char *type_filter;
    const char *extension;
    int max_depth;
    int min_depth;
    int exact_depth;
    int max_results;
    int results;
    int batch_size;
    bool batch_size_set;
    enum fd_exec_mode exec_mode;
    const char **exec_argv;
    int exec_argc;
};

struct fd_state {
    struct fd_opts *opts;
    pcre2_code *regexes[FD_MAX_AND_PATTERNS + 1];
    pcre2_match_data *match_data[FD_MAX_AND_PATTERNS + 1];
    int regex_count;
    bool *stop;
    bool strip_implicit_dot_prefix;
    char *cwd;
    struct fd_exec_items exec_items;
    bool exec_collect_failed;
};

static bool fd_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > (1 << 20)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool fd_parse_strip_cwd_prefix(const char *progname, const char *text,
                                      enum fd_strip_cwd_prefix_mode *out) {
    if (!text || strcmp(text, "auto") == 0) {
        *out = FD_STRIP_CWD_PREFIX_AUTO;
        return true;
    }
    if (strcmp(text, "always") == 0) {
        *out = FD_STRIP_CWD_PREFIX_ALWAYS;
        return true;
    }
    if (strcmp(text, "never") == 0) {
        *out = FD_STRIP_CWD_PREFIX_NEVER;
        return true;
    }

    fprintf(stderr, "%s: invalid argument for --strip-cwd-prefix: %s\n", progname, text);
    return false;
}

static bool fd_exec_items_append(struct fd_exec_items *items, char *text) {
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

static void fd_exec_items_free(struct fd_exec_items *items) {
    if (!items)
        return;

    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    items->v = NULL;
    items->count = 0;
    items->cap = 0;
}

static int fd_find_exec_option(int argc, char **argv, enum fd_exec_mode *mode,
                               int *command_start, const char **inline_command) {
    bool end_of_options = false;

    *mode = FD_EXEC_NONE;
    *command_start = argc;
    *inline_command = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_of_options && strcmp(arg, "--") == 0) {
            end_of_options = true;
            continue;
        }
        if (end_of_options)
            continue;

        if (strcmp(arg, "-x") == 0 || strcmp(arg, "--exec") == 0) {
            *mode = FD_EXEC_EACH;
            *command_start = i + 1;
            return i;
        }
        if (strcmp(arg, "-X") == 0 || strcmp(arg, "--exec-batch") == 0) {
            *mode = FD_EXEC_BATCH;
            *command_start = i + 1;
            return i;
        }
        if (strncmp(arg, "--exec=", 7) == 0) {
            *mode = FD_EXEC_EACH;
            *command_start = i + 1;
            *inline_command = arg + 7;
            return i;
        }
        if (strncmp(arg, "--exec-batch=", 13) == 0) {
            *mode = FD_EXEC_BATCH;
            *command_start = i + 1;
            *inline_command = arg + 13;
            return i;
        }
    }

    return argc;
}

static uint32_t fd_compile_flags(const struct fd_opts *opts, const char *pattern) {
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

static const char *fd_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool fd_parse_type_filter(const char *progname, const char *text, const char **out) {
    char type_filter = '\0';
    if (!bx_walk_parse_named_type_filter(text, &type_filter)) {
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname, text);
        return false;
    }

    switch (type_filter) {
    case 'f': *out = "f"; break;
    case 'd': *out = "d"; break;
    case 'l': *out = "l"; break;
    case 'x': *out = "x"; break;
    case 'e': *out = "e"; break;
    case 'p': *out = "p"; break;
    case 's': *out = "s"; break;
    case 'b': *out = "b"; break;
    case 'c': *out = "c"; break;
    default:
        fprintf(stderr, "%s: invalid argument for --type: %s\n", progname, text);
        return false;
    }
    return true;
}

static bool fd_matches_type(struct walk_entry *entry, const struct fd_opts *opts) {
    if (!opts->type_filter)
        return true;
    return bx_walk_entry_matches_type(entry, opts->type_filter[0]);
}

static pcre2_code *fd_compile_regex(const char *progname, const char *pattern,
                                    const char *display_pattern, uint32_t flags) {
    int errcode;
    PCRE2_SIZE erroffset;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                                   flags, &errcode, &erroffset, NULL);
    if (re)
        return re;

    PCRE2_UCHAR errbuf[256];
    int msg_rc = pcre2_get_error_message(errcode, errbuf, sizeof(errbuf));
    fprintf(stderr, "%s: invalid pattern '%s': regex parse error at offset %zu: %s\n",
            progname,
            display_pattern ? display_pattern : pattern,
            (size_t)erroffset,
            msg_rc >= 0 ? (const char *)errbuf : "regex compile failed");
    return NULL;
}

static pcre2_code *fd_compile_pattern(const char *progname, const struct fd_opts *opts,
                                      const char *pattern) {
    if (!pattern)
        return NULL;

    if (!opts->glob_match && !opts->fixed_strings) {
        uint32_t flags = fd_compile_flags(opts, pattern);
        return fd_compile_regex(progname, pattern, pattern, flags);
    }

    if (opts->glob_match) {
        char buf[4096];
        char *p = buf;
        for (const char *ch = pattern; *ch; ch++) {
            switch (*ch) {
            case '*': *p++ = '.'; *p++ = '*'; break;
            case '?': *p++ = '.'; break;
            case '.': *p++ = '\\'; *p++ = '.'; break;
            default:  *p++ = *ch; break;
            }
        }
        *p = '\0';
        return fd_compile_regex(progname, buf, pattern, fd_compile_flags(opts, pattern));
    }

    size_t len = strlen(pattern);
    const char *raw = pattern;
    char *buf = malloc(len * 2 + 3);
    if (!buf)
        return NULL;
    char *p = buf;
    for (size_t i = 0; i < len; i++) {
        char ch = raw[i];
        if (ch == '.' || ch == '\\' || ch == '+' || ch == '*' || ch == '?' ||
            ch == '[' || ch == ']' || ch == '(' || ch == ')' || ch == '{' ||
            ch == '}' || ch == '^' || ch == '$' || ch == '|')
            *p++ = '\\';
        *p++ = ch;
    }
    *p = '\0';
    pcre2_code *re = fd_compile_regex(progname, buf, pattern, fd_compile_flags(opts, pattern));
    free(buf);
    return re;
}

static bool fd_match_name(const struct fd_state *st, const char *name) {
    if (st->regex_count == 0)
        return true;

    for (int i = 0; i < st->regex_count; i++) {
        int rc = pcre2_match(st->regexes[i], (PCRE2_SPTR)name, strlen(name), 0, 0,
                             st->match_data[i], NULL);
        if (rc < 0)
            return false;
    }
    return true;
}

static void fd_print_path(const struct fd_state *st, const char *path);

static bool fd_match_placeholder_raw(const char *text, enum fd_placeholder_kind *kind, size_t *len) {
    if (strncmp(text, "{//}", 4) == 0) {
        *kind = FD_PH_DIRNAME;
        *len = 4;
        return true;
    }
    if (strncmp(text, "{/.}", 4) == 0) {
        *kind = FD_PH_BASENAME_STEM;
        *len = 4;
        return true;
    }
    if (strncmp(text, "{/}", 3) == 0) {
        *kind = FD_PH_BASENAME;
        *len = 3;
        return true;
    }
    if (strncmp(text, "{.}", 3) == 0) {
        *kind = FD_PH_PATH_STEM;
        *len = 3;
        return true;
    }
    if (strncmp(text, FD_PLACEHOLDER, sizeof(FD_PLACEHOLDER) - 1) == 0) {
        *kind = FD_PH_PATH;
        *len = sizeof(FD_PLACEHOLDER) - 1;
        return true;
    }
    return false;
}

static bool fd_match_placeholder_at(const char *arg, const char *text,
                                    enum fd_placeholder_kind *kind, size_t *len) {
    if (!fd_match_placeholder_raw(text, kind, len))
        return false;

    if (text > arg && text[-1] == '{')
        return false;
    if (text[*len] == '}')
        return false;
    return true;
}

static size_t fd_placeholder_count(const char *arg) {
    size_t count = 0;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            (void)kind;
            count++;
            p += len;
            continue;
        }
        p++;
    }
    return count;
}

static char *fd_strndup(const char *text, size_t len) {
    char *out = malloc(len + 1);
    if (!out)
        return NULL;
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

static const char *fd_stem_input_path(const char *path) {
    if (path[0] == '.' && path[1] == '/')
        return path + 2;
    return path;
}

static char *fd_remove_last_extension(const char *path) {
    const char *base = fd_basename(path);
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base)
        return strdup(path);
    return fd_strndup(path, (size_t)(dot - path));
}

static char *fd_placeholder_value(enum fd_placeholder_kind kind, const char *path) {
    switch (kind) {
    case FD_PH_PATH:
        return strdup(path);
    case FD_PH_BASENAME:
        return strdup(fd_basename(path));
    case FD_PH_DIRNAME: {
        const char *slash = strrchr(path, '/');
        if (!slash)
            return strdup(".");
        if (slash == path)
            return fd_strndup(path, 1);
        return fd_strndup(path, (size_t)(slash - path));
    }
    case FD_PH_PATH_STEM:
        return fd_remove_last_extension(fd_stem_input_path(path));
    case FD_PH_BASENAME_STEM:
        return fd_remove_last_extension(fd_basename(fd_stem_input_path(path)));
    case FD_PH_NONE:
        break;
    }
    return NULL;
}

static char *fd_expand_placeholders(const char *arg, const char *path) {
    size_t out_len = 1;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            out_len++;
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            out_len++;
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            char *value = fd_placeholder_value(kind, path);
            if (!value)
                return NULL;
            out_len += strlen(value);
            free(value);
            p += len;
            continue;
        }
        out_len++;
        p++;
    }

    char *out = malloc(out_len);
    if (!out)
        return NULL;

    char *dst = out;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            *dst++ = '{';
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            *dst++ = '}';
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            char *value = fd_placeholder_value(kind, path);
            size_t value_len;
            if (!value) {
                free(out);
                return NULL;
            }
            value_len = strlen(value);
            memcpy(dst, value, value_len);
            dst += value_len;
            free(value);
            p += len;
            continue;
        }
        *dst++ = *p++;
    }
    *dst = '\0';
    return out;
}

static char *fd_exec_path(const struct fd_state *st, const char *path) {
    const char *relative = path;

    if (!st->opts->absolute_path)
        return NULL;

    if (relative[0] == '.' && relative[1] == '/')
        relative += 2;
    if (relative[0] == '/')
        return strdup(relative);

    if (!st->cwd || st->cwd[0] == '\0')
        return strdup(relative);

    size_t cwd_len = strlen(st->cwd);
    size_t rel_len = strlen(relative);
    char *out = malloc(cwd_len + 1 + rel_len + 1);
    if (!out)
        return NULL;
    memcpy(out, st->cwd, cwd_len);
    out[cwd_len] = '/';
    memcpy(out + cwd_len + 1, relative, rel_len + 1);
    return out;
}

static char *fd_apply_path_separator(const struct fd_opts *opts, const char *path) {
    const char *sep = opts->path_separator;
    if (!sep || strcmp(sep, "/") == 0)
        return strdup(path);

    size_t sep_len = strlen(sep);
    size_t slash_count = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            slash_count++;
    }

    size_t path_len = strlen(path);
    size_t out_len = path_len + slash_count * sep_len;
    if (slash_count > 0)
        out_len -= slash_count;
    char *out = malloc(out_len + 1);
    if (!out)
        return NULL;

    char *dst = out;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            memcpy(dst, sep, sep_len);
            dst += sep_len;
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';
    return out;
}

static bool fd_should_strip_cwd_prefix(const struct fd_state *st, bool for_exec) {
    if (st->opts->absolute_path)
        return false;

    switch (st->opts->strip_cwd_prefix) {
    case FD_STRIP_CWD_PREFIX_ALWAYS:
    case FD_STRIP_CWD_PREFIX_AUTO:
        return st->strip_implicit_dot_prefix;
    case FD_STRIP_CWD_PREFIX_NEVER:
        return false;
    case FD_STRIP_CWD_PREFIX_UNSET:
        return !for_exec && st->strip_implicit_dot_prefix;
    }
    return false;
}

static char *fd_render_output_path(const struct fd_state *st, const char *path) {
    char *base = NULL;
    if (st->opts->absolute_path) {
        base = fd_exec_path(st, path);
    } else {
        const char *relative = path;
        if (fd_should_strip_cwd_prefix(st, false) && relative[0] == '.' && relative[1] == '/')
            relative += 2;
        base = strdup(relative);
    }
    if (!base)
        return NULL;

    char *rendered = fd_apply_path_separator(st->opts, base);
    free(base);
    return rendered;
}

static char *fd_render_exec_path(const struct fd_state *st, const char *path) {
    char *base;
    if (st->opts->absolute_path) {
        base = fd_exec_path(st, path);
    } else {
        const char *relative = path;
        if (fd_should_strip_cwd_prefix(st, true) && relative[0] == '.' && relative[1] == '/')
            relative += 2;
        base = strdup(relative);
    }
    char *rendered;
    if (!base)
        return NULL;
    rendered = fd_apply_path_separator(st->opts, base);
    free(base);
    return rendered;
}

static bool fd_record_match(struct fd_state *st, const char *path) {
    struct fd_opts *opts = st->opts;
    opts->results++;

    if (opts->exec_mode != FD_EXEC_NONE) {
        char *exec_path = fd_render_exec_path(st, path);
        if (!exec_path || !fd_exec_items_append(&st->exec_items, exec_path)) {
            free(exec_path);
            st->exec_collect_failed = true;
            if (st->stop)
                *st->stop = true;
            return false;
        }
    } else if (!opts->quiet) {
        fd_print_path(st, path);
    }

    if ((opts->max_results > 0 && opts->results >= opts->max_results) ||
        (opts->quiet && opts->results > 0)) {
        if (st->stop)
            *st->stop = true;
    }
    return true;
}

static void fd_print_path(const struct fd_state *st, const char *path) {
    const struct fd_opts *opts = st->opts;
    char terminator = opts->print0 ? '\0' : '\n';
    char *rendered = fd_render_output_path(st, path);
    if (!rendered)
        return;
    printf("%s%c", rendered, terminator);
    free(rendered);
}

static size_t fd_batch_argv_bytes(const char **command_argv, int command_argc,
                                  char **items, int start, int count,
                                  bool batch_mode, bool *saw_placeholder) {
    size_t total = 0;
    bool saw = false;

    for (int i = 0; i < command_argc; i++) {
        size_t marker_count = fd_placeholder_count(command_argv[i]);
        if (marker_count == 0) {
            char *expanded = fd_expand_placeholders(command_argv[i], "");
            if (!expanded)
                return (size_t)-1;
            total += strlen(expanded) + 1;
            free(expanded);
            continue;
        }

        saw = true;
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            char *expanded = fd_expand_placeholders(command_argv[i], items[start + j]);
            if (!expanded)
                return (size_t)-1;
            total += strlen(expanded) + 1;
            free(expanded);
        }
    }

    if (!saw) {
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++)
            total += strlen(items[start + j]) + 1;
    }

    if (saw_placeholder)
        *saw_placeholder = saw;
    return total;
}

static int fd_select_exec_batch_count(const char **command_argv, int command_argc,
                                      char **items, int item_count, int start,
                                      size_t char_limit) {
    int take = 0;

    while (start + take < item_count) {
        size_t bytes = fd_batch_argv_bytes(command_argv, command_argc,
                                           items, start, take + 1, true, NULL);
        if (bytes == (size_t)-1)
            return -1;
        if (char_limit > 0 && bytes > char_limit)
            break;
        take++;
    }

    return take > 0 ? take : -1;
}

static char **fd_build_exec_argv(const char **command_argv, int command_argc,
                                 char **items, int start, int count,
                                 bool batch_mode) {
    int capacity = command_argc + (count > 0 ? count * command_argc : 0) + count + 1;
    char **argv = calloc((size_t)capacity, sizeof(*argv));
    if (!argv)
        return NULL;

    int argc = 0;
    bool saw_placeholder = false;
    for (int i = 0; i < command_argc; i++) {
        size_t marker_count = fd_placeholder_count(command_argv[i]);
        if (marker_count == 0) {
            argv[argc] = fd_expand_placeholders(command_argv[i], "");
            if (!argv[argc])
                goto fail;
            argc++;
            continue;
        }

        saw_placeholder = true;
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            argv[argc] = fd_expand_placeholders(command_argv[i], items[start + j]);
            if (!argv[argc])
                goto fail;
            argc++;
        }
    }

    if (!saw_placeholder) {
        int item_total = batch_mode ? count : (count > 0 ? 1 : 0);
        for (int j = 0; j < item_total; j++) {
            argv[argc] = strdup(items[start + j]);
            if (!argv[argc])
                goto fail;
            argc++;
        }
    }

    argv[argc] = NULL;
    return argv;

fail:
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
    return NULL;
}

static void fd_free_exec_argv(char **argv) {
    if (!argv)
        return;
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

static int fd_count_placeholder_args(const struct fd_opts *opts) {
    int count = 0;
    for (int i = 0; i < opts->exec_argc; i++) {
        if (fd_placeholder_count(opts->exec_argv[i]) > 0)
            count++;
    }
    return count;
}

struct fd_exec_reap_ctx {
    const char *progname;
    const char *cmdname;
    int *final_rc;
};

static void fd_exec_reap_status_cb(pid_t pid, int status, bool exec_failed, int exec_errno,
                                   void *user) {
    (void)pid;
    struct fd_exec_reap_ctx *ctx = user;
    if (exec_failed) {
        fprintf(stderr, "%s: failed to run command '%s': %s\n",
                ctx->progname, ctx->cmdname, strerror(exec_errno));
        *ctx->final_rc = 1;
        return;
    }

    if ((WIFEXITED(status) && WEXITSTATUS(status) != 0) || WIFSIGNALED(status))
        *ctx->final_rc = 1;
}

static int fd_run_exec_commands(const char *progname, struct fd_state *st) {
    if (st->opts->exec_mode == FD_EXEC_NONE || st->exec_items.count == 0)
        return 0;

    size_t char_limit = bx_argv_effective_char_limit(0);
    int final_rc = 0;
    int running = 0;
    struct bx_child child = {0};
    struct bx_child_runner_opts runner_opts = {0};
    int i = 0;

    while (i < st->exec_items.count) {
        int take = 1;
        if (st->opts->exec_mode == FD_EXEC_BATCH) {
            take = fd_select_exec_batch_count(st->opts->exec_argv, st->opts->exec_argc,
                                              st->exec_items.v, st->exec_items.count, i,
                                              char_limit);
            if (take < 0) {
                fprintf(stderr, "%s: argument line too long\n", progname);
                return 1;
            }
            if (st->opts->batch_size > 0 && take > st->opts->batch_size)
                take = st->opts->batch_size;
        } else if (char_limit > 0) {
            size_t bytes = fd_batch_argv_bytes(st->opts->exec_argv, st->opts->exec_argc,
                                               st->exec_items.v, i, 1, false, NULL);
            if (bytes == (size_t)-1)
                return 1;
            if (bytes > char_limit) {
                fprintf(stderr, "%s: argument line too long\n", progname);
                return 1;
            }
        }

        char **argv = fd_build_exec_argv(st->opts->exec_argv, st->opts->exec_argc,
                                         st->exec_items.v, i, take,
                                         st->opts->exec_mode == FD_EXEC_BATCH);
        if (!argv)
            return 1;

        bool exec_failed_now = false;
        int exec_errno_now = 0;
        int spawn_rc = bx_child_spawn_argv(progname, argv, &runner_opts, 0,
                                           &child, &running,
                                           &exec_failed_now, &exec_errno_now);
        fd_free_exec_argv(argv);
        if (spawn_rc != 0)
            return 1;
        if (exec_failed_now)
            final_rc = 1;

        struct fd_exec_reap_ctx ctx = {
            .progname = progname,
            .cmdname = st->opts->exec_argv[0],
            .final_rc = &final_rc,
        };
        if (bx_child_reap(&child, &running, true, true, fd_exec_reap_status_cb, &ctx) != 0)
            return 1;

        if (exec_failed_now)
            break;
        i += take;
    }

    return final_rc;
}

static void fd_callback(struct walk_entry *entry, void *user) {
    struct fd_state *st = user;
    struct fd_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;

    if (opts->max_results > 0 && opts->results >= opts->max_results) {
        if (st->stop) *st->stop = true;
        return;
    }

    if (opts->quiet && opts->results > 0) {
        if (st->stop) *st->stop = true;
        return;
    }

    if (entry->depth == 0 && entry->is_dir)
        return;

    if (opts->exact_depth >= 0) {
        if (entry->depth != opts->exact_depth)
            return;
    } else if (entry->depth < opts->min_depth) {
        return;
    }

    if (!fd_matches_type(entry, opts))
        return;

    const char *name = opts->full_path ? entry->path : fd_basename(entry->path);

    if (opts->extension) {
        const char *dot = strrchr(fd_basename(entry->path), '.');
        if (!dot || strcasecmp(dot + 1, opts->extension) != 0)
            return;
    }

    if (st->regex_count == 0) {
        fd_record_match(st, entry->path);
        return;
    }

    if (fd_match_name(st, name)) {
        fd_record_match(st, entry->path);
    }
}

int bx_fd_main(int argc, char **argv) {
    struct fd_opts opts = {0};
    opts.max_depth = -1;
    opts.exact_depth = -1;
    opts.smart_case = true;
    bool show_help = false;
    const char *progname = argv[0] ? argv[0] : "fd";
    int parse_argc = argc;
    const char *inline_exec_command = NULL;
    int exec_command_start = argc;
    const char **exec_argv_storage = NULL;

    parse_argc = fd_find_exec_option(argc, argv, &opts.exec_mode,
                                     &exec_command_start, &inline_exec_command);
    if (opts.exec_mode != FD_EXEC_NONE) {
        int exec_argc = argc - exec_command_start + (inline_exec_command ? 1 : 0);
        if (exec_argc <= 0 || (inline_exec_command && inline_exec_command[0] == '\0')) {
            fprintf(stderr, "%s: %s requires a command\n",
                    progname, opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch" : "--exec");
            return 2;
        }
        exec_argv_storage = calloc((size_t)exec_argc + 1, sizeof(*exec_argv_storage));
        if (!exec_argv_storage)
            return 1;
        if (inline_exec_command) {
            exec_argv_storage[0] = inline_exec_command;
            for (int i = 1; i < exec_argc; i++)
                exec_argv_storage[i] = argv[exec_command_start + i - 1];
        } else {
            for (int i = 0; i < exec_argc; i++)
                exec_argv_storage[i] = argv[exec_command_start + i];
        }
        opts.exec_argv = exec_argv_storage;
        opts.exec_argc = exec_argc;
    }

    int opt;
    static struct option long_opts[] = {
        {"help",     no_argument,       NULL, 'h'},
        {"version",  no_argument,       NULL, 'V'},
        {"hidden",   no_argument,       NULL, 'H'},
        {"no-ignore", no_argument,      NULL, 'I'},
        {"absolute-path", no_argument,  NULL, 'a'},
        {"relative-path", no_argument,  NULL, 205},
        {"follow",   no_argument,       NULL, 'L'},
        {"full-path", no_argument,      NULL, 'p'},
        {"ignore-case", no_argument,    NULL, 'i'},
        {"case-sensitive", no_argument, NULL, 's'},
        {"fixed-strings", no_argument,  NULL, 'F'},
        {"glob",      no_argument,      NULL, 'g'},
        {"regex",     no_argument,      NULL, 204},
        {"max-depth", required_argument, NULL, 'd'},
        {"min-depth", required_argument, NULL, 201},
        {"exact-depth", required_argument, NULL, 202},
        {"type",      required_argument, NULL, 't'},
        {"extension", required_argument, NULL, 'e'},
        {"max-results", required_argument, NULL, 200},
        {"and",       required_argument, NULL, 203},
        {"path-separator", required_argument, NULL, 207},
        {"batch-size", required_argument, NULL, 206},
        {"show-errors", no_argument, NULL, 209},
        {"strip-cwd-prefix", optional_argument, NULL, 208},
        {"exec",      required_argument, NULL, 'x'},
        {"exec-batch", required_argument, NULL, 'X'},
        {"print0",    no_argument,      NULL, '0'},
        {"quiet",     no_argument,      NULL, 'q'},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    while ((opt = getopt_long(parse_argc, argv, "hVHIapisSFgd:t:e:x:X:0qL1", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'h': show_help = true; break;
        case 'V':
            printf("fd (bx) %s\n", BX_VERSION);
            return 0;
        case 'H': opts.hidden = true; break;
        case 'I': opts.no_ignore = true; break;
        case 'a': opts.absolute_path = true; break;
        case 205: opts.absolute_path = false; break;
        case 'p': opts.full_path = true; break;
        case 'i': opts.ignore_case = true; opts.smart_case = false; break;
        case 's': opts.case_sensitive = true; opts.smart_case = false; break;
        case 'F': opts.fixed_strings = true; break;
        case 'g': opts.glob_match = true; break;
        case 204:
            opts.fixed_strings = false;
            opts.glob_match = false;
            break;
        case 'd':
            if (!fd_parse_nonnegative_int(progname, "--max-depth", optarg, &opts.max_depth))
                return 2;
            break;
        case 201:
            if (!fd_parse_nonnegative_int(progname, "--min-depth", optarg, &opts.min_depth))
                return 2;
            break;
        case 202:
            if (!fd_parse_nonnegative_int(progname, "--exact-depth", optarg, &opts.exact_depth))
                return 2;
            opts.max_depth = opts.exact_depth;
            break;
        case 't':
            if (!fd_parse_type_filter(progname, optarg, &opts.type_filter))
                return 2;
            break;
        case 'e': opts.extension = optarg; break;
        case '0': opts.print0 = true; break;
        case 'q': opts.quiet = true; break;
        case 'L': opts.follow_symlinks = true; break;
        case '1': opts.max_results = 1; break;
        case 'x':
        case 'X':
            break;
        case 206:
            if (!fd_parse_nonnegative_int(progname, "--batch-size", optarg, &opts.batch_size))
                return 2;
            opts.batch_size_set = true;
            break;
        case 207:
            opts.path_separator = optarg;
            break;
        case 209:
            opts.show_errors = true;
            break;
        case 208:
            if (!fd_parse_strip_cwd_prefix(progname, optarg, &opts.strip_cwd_prefix))
                return 2;
            break;
        case 203:
            if (opts.num_and_patterns < FD_MAX_AND_PATTERNS)
                opts.and_patterns[opts.num_and_patterns++] = optarg;
            break;
        case 200:
            if (!fd_parse_nonnegative_int(progname, "--max-results", optarg, &opts.max_results))
                return 2;
            break;
        case '?':
            if (optind > 0 && optind <= argc)
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
            else
                fprintf(stderr, "%s: unrecognized option\n", progname);
            return 2;
        }
    }

    if (show_help) {
        printf("Usage: %s [OPTIONS] [PATTERN] [PATH]...\n", argv[0]);
        puts("fd - find entries in the filesystem");
        puts("");
        puts("  -H, --hidden        search hidden files and directories");
        puts("  -I, --no-ignore     do not respect ignore files");
        puts("  -a, --absolute-path show absolute paths");
        puts("      --relative-path show relative paths");
        puts("      --path-separator SEP replace '/' in rendered paths with SEP");
        puts("      --show-errors    print permission and traversal errors");
        puts("      --strip-cwd-prefix[=WHEN] control leading ./ rendering (auto, always, never)");
        puts("  -p, --full-path     match against full path, not basename");
        puts("  -i, --ignore-case   case-insensitive matching");
        puts("  -s, --case-sensitive  case-sensitive matching");
        puts("  -F, --fixed-strings treat pattern as literal string");
        puts("  -g, --glob          glob-based matching");
        puts("      --regex         treat pattern as a regular expression");
        puts("      --and PATTERN   require PATTERN to match too");
        puts("  -d, --max-depth N   limit recursive depth");
        puts("      --min-depth N   skip matches shallower than N");
        puts("      --exact-depth N match only entries exactly at depth N");
        puts("  -t, --type TYPE     filter by type: f,d,l,x,e,p,s,b,c");
        puts("  -e, --extension EXT filter by file extension");
        puts("  -0, --print0        separate results by NUL byte");
        puts("  -q, --quiet         suppress normal output");
        puts("  -1                  alias for --max-results=1");
        puts("  -L, --follow        follow symlinks");
        puts("  -x, --exec CMD ...  run CMD once per search result");
        puts("  -X, --exec-batch CMD ... run CMD once with batched search results");
        puts("      --batch-size N   limit results per --exec-batch invocation");
        puts("      --max-results N limit number of results");
        puts("      --help           display this help and exit");
        puts("      --version        output version information and exit");
        return 0;
    }

    opts.pattern = NULL;
    bool using_implicit_root = true;
    char *default_paths[] = { "." };
    char **search_paths = default_paths;
    int search_path_count = 1;
    int positional = parse_argc - optind;
    if (positional == 1) {
        opts.pattern = argv[optind++];
    } else if (positional > 1) {
        opts.pattern = argv[optind++];
        search_paths = &argv[optind];
        search_path_count = parse_argc - optind;
        using_implicit_root = false;
    }

    if (opts.strip_cwd_prefix != FD_STRIP_CWD_PREFIX_UNSET && !using_implicit_root) {
        fprintf(stderr, "%s: --strip-cwd-prefix cannot be used with explicit search paths\n",
                progname);
        free(exec_argv_storage);
        return 2;
    }

    if (opts.exec_mode != FD_EXEC_NONE) {
        if (opts.quiet) {
            fprintf(stderr, "%s: --quiet cannot be used with %s\n",
                    progname, opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch" : "--exec");
            return 2;
        }
        if (opts.max_results > 0) {
            fprintf(stderr, "%s: --max-results cannot be used with %s\n",
                    progname, opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch" : "--exec");
            return 2;
        }
        if (opts.print0) {
            fprintf(stderr, "%s: --print0 cannot be used with %s\n",
                    progname, opts.exec_mode == FD_EXEC_BATCH ? "--exec-batch" : "--exec");
            return 2;
        }
        if (opts.exec_mode == FD_EXEC_BATCH && fd_count_placeholder_args(&opts) > 1) {
            fprintf(stderr, "%s: only one placeholder-bearing argument is allowed with --exec-batch\n",
                    progname);
            return 2;
        }
    } else if (opts.batch_size_set) {
        fprintf(stderr, "%s: --batch-size requires --exec-batch\n", progname);
        return 2;
    }

    bool stop = false;
    struct walk_opts wopts = {
        .hidden = opts.hidden,
        .no_ignore = opts.no_ignore,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = true,
        .stop = &stop,
        .suppress_eacces = true,
        .report_eacces = opts.show_errors,
        .os_error_style = opts.show_errors,
        .error_prefix = opts.show_errors ? "[fd error]" : progname,
        .max_depth = opts.max_depth,
        .cycle_mode = opts.follow_symlinks ? WALK_CYCLE_SYMLINK_REPEAT : WALK_CYCLE_NONE,
        .cycle_report = WALK_CYCLE_IGNORE,
    };

    struct fd_state state = {
        .opts = &opts,
        .stop = &stop,
        .strip_implicit_dot_prefix = using_implicit_root,
    };
    if (opts.absolute_path)
        state.cwd = getcwd(NULL, 0);
    if (opts.pattern)
        state.regexes[state.regex_count++] = fd_compile_pattern(progname, &opts, opts.pattern);
    for (int i = 0; i < opts.num_and_patterns; i++)
        state.regexes[state.regex_count++] = fd_compile_pattern(progname, &opts, opts.and_patterns[i]);
    if (!opts.pattern && opts.num_and_patterns > 0) {
        for (int i = 0; i < state.regex_count; i++) {
            if (!state.regexes[i])
                return 1;
        }
    } else if (opts.pattern) {
        for (int i = 0; i < state.regex_count; i++) {
            if (!state.regexes[i])
                return 1;
        }
    }
    for (int i = 0; i < state.regex_count; i++)
        state.match_data[i] = pcre2_match_data_create_from_pattern(state.regexes[i], NULL);
    int walk_rc = 0;
    for (int i = 0; i < search_path_count && !stop; i++) {
        if (walk_dir(search_paths[i], &wopts, fd_callback, &state) != 0)
            walk_rc = -1;
    }

    for (int i = 0; i < state.regex_count; i++) {
        if (state.match_data[i])
            pcre2_match_data_free(state.match_data[i]);
        if (state.regexes[i])
            pcre2_code_free(state.regexes[i]);
    }
    int exec_rc = 0;
    if (!state.exec_collect_failed && walk_rc == 0 && opts.exec_mode != FD_EXEC_NONE)
        exec_rc = fd_run_exec_commands(progname, &state);
    fd_exec_items_free(&state.exec_items);
    free(state.cwd);
    free(exec_argv_storage);
    if (state.exec_collect_failed)
        return 1;
    if (walk_rc != 0)
        return 1;
    if (opts.exec_mode != FD_EXEC_NONE)
        return exec_rc;
    if (opts.quiet)
        return opts.results > 0 ? 0 : 1;
    return 0;
}
