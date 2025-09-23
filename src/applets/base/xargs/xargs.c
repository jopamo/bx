#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
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

struct xargs_opts {
    bool no_run_if_empty;
    bool nul_delim;
    bool delimiter_mode;
    bool exit_if_too_big;
    bool open_tty;
    bool interactive;
    bool verbose;
    char delimiter;
    int max_args;
    int max_lines;
    int max_chars;
    int max_procs;
    const char *arg_file;
    const char *logical_eof;
    bool replace_mode;
    const char *replace_marker;
    const char *process_slot_var;
};

struct xargs_items {
    char **v;
    int *line_groups;
    int count;
    int cap;
};

static void xargs_warn_mutex(const char *progname, const char *left, const char *right,
                             const char *ignored) {
    fprintf(stderr,
            "%s: warning: options %s and %s are mutually exclusive, ignoring previous %s value\n",
            progname, left, right, ignored);
}

static void xargs_disable_replace(struct xargs_opts *opts) {
    opts->replace_mode = false;
    opts->replace_marker = NULL;
}

static void xargs_set_max_args(struct xargs_opts *opts, const char *progname, int value) {
    if (opts->replace_mode) {
        xargs_warn_mutex(progname, "--replace", "--max-args/-n", "--replace");
        xargs_disable_replace(opts);
    }
    if (opts->max_lines > 0) {
        xargs_warn_mutex(progname, "--max-lines", "--max-args/-n", "--max-lines");
        opts->max_lines = 0;
    }
    opts->max_args = value;
}

static void xargs_set_max_lines(struct xargs_opts *opts, const char *progname,
                                const char *optname, int value) {
    if (opts->replace_mode) {
        xargs_warn_mutex(progname, "--replace", optname, "--replace");
        xargs_disable_replace(opts);
    }
    if (opts->max_args > 0) {
        xargs_warn_mutex(progname, "--max-args", optname, "--max-args");
        opts->max_args = 0;
    }
    opts->max_lines = value;
}

static void xargs_set_replace_mode(struct xargs_opts *opts, const char *progname,
                                   const char *marker) {
    if (opts->max_args > 0) {
        xargs_warn_mutex(progname, "--max-args", "--replace/-I/-i", "--max-args");
        opts->max_args = 0;
    }
    if (opts->max_lines > 0) {
        xargs_warn_mutex(progname, "--max-lines", "--replace/-I/-i", "--max-lines");
        opts->max_lines = 0;
    }
    opts->replace_mode = true;
    opts->replace_marker = marker;
}

static void xargs_print_help(const char *progname) {
    printf("Usage: %s [OPTION]... [COMMAND [INITIAL-ARGS]...]\n", progname);
    puts("Run COMMAND with arguments read from standard input.");
    puts("");
    puts("  -0, --null              items are separated by NUL, not whitespace");
    puts("  -a, --arg-file=FILE     read items from FILE instead of standard input");
    puts("  -d, --delimiter=CHAR    items are separated by CHAR");
    puts("  -E, --eof=END           stop reading input after END");
    puts("  -n, --max-args=MAX      use at most MAX input items per command line");
    puts("  -L, --max-lines=MAX     use at most MAX input lines per command line");
    puts("  -l[MAX]                 like -L, defaulting MAX to 1");
    puts("  -I, --replace=R         replace R in initial arguments with each input item");
    puts("  -i[R]                   like -I, defaulting R to {}");
    puts("  -o, --open-tty          reopen standard input as /dev/tty in the child");
    puts("  -p, --interactive       prompt before running commands");
    puts("  -t, --verbose           print each command before running it");
    puts("      --process-slot-var=VAR  set VAR to the worker slot number in each child");
    puts("  -s, --max-chars=MAX     use at most MAX command-line bytes");
    puts("  -x, --exit              fail if a command line would be too large");
    puts("  -P, --max-procs=MAX     run at most MAX commands at a time");
    puts("  -r, --no-run-if-empty   do not run command if there are no items");
    puts("      --help              display this help and exit");
    puts("      --show-limits       display command-line limits");
    puts("      --version           output version information and exit");
}

static void xargs_print_version(void) {
    printf("xargs (bx) %s\n", BX_VERSION);
}

static void xargs_print_limits(void) {
    long arg_max = sysconf(_SC_ARG_MAX);
    if (arg_max < 0)
        arg_max = 0;

    size_t env_bytes = bx_argv_environment_bytes();

    printf("Your environment variables take up %zu bytes\n", env_bytes);
    printf("POSIX upper limit on argument length (this system): %ld\n", arg_max);
    if (arg_max > 0 && env_bytes < (size_t)arg_max)
        printf("Maximum command length we could try to use: %ld\n", arg_max - (long)env_bytes);
}

static bool xargs_parse_int(const char *progname, const char *optname, const char *text, int *out,
                            bool allow_zero) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    long min = allow_zero ? 0 : 1;
    if (!text || *text == '\0' || (end && *end != '\0') || v < min || v > 100000) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool xargs_parse_delimiter(const char *progname, const char *text, char *out) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text ? text : "(null)");
        return false;
    }

    if (text[0] != '\\') {
        if (text[1] != '\0') {
            fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
            return false;
        }
        *out = text[0];
        return true;
    }

    if (text[1] == '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
        return false;
    }

    if (text[1] == 'x') {
        char *end = NULL;
        long v = strtol(text + 2, &end, 16);
        if (!end || end == text + 2 || *end != '\0' || v < 0 || v > 255) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname, text);
            return false;
        }
        *out = (char)v;
        return true;
    }

    if (text[1] >= '0' && text[1] <= '7') {
        char *end = NULL;
        long v = strtol(text + 1, &end, 8);
        if (!end || end == text + 1 || *end != '\0' || v < 0 || v > 255) {
            fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname, text);
            return false;
        }
        *out = (char)v;
        return true;
    }

    switch (text[1]) {
    case 'n': *out = '\n'; break;
    case 't': *out = '\t'; break;
    case 'r': *out = '\r'; break;
    case '\\': *out = '\\'; break;
    default:
        fprintf(stderr, "%s: invalid delimiter escape: %s\n", progname, text);
        return false;
    }
    if (text[2] != '\0') {
        fprintf(stderr, "%s: invalid delimiter: %s\n", progname, text);
        return false;
    }
    return true;
}

static bool xargs_items_append(struct xargs_items *items, const char *text, int line_group) {
    if (items->count >= items->cap) {
        int new_cap = items->cap == 0 ? 16 : items->cap * 2;
        char **tmp = realloc(items->v, (size_t)new_cap * sizeof(*items->v));
        if (!tmp)
            return false;
        items->v = tmp;
        int *line_tmp = realloc(items->line_groups, (size_t)new_cap * sizeof(*items->line_groups));
        if (!line_tmp)
            return false;
        items->line_groups = line_tmp;
        items->cap = new_cap;
    }
    items->v[items->count] = strdup(text);
    if (!items->v[items->count])
        return false;
    items->line_groups[items->count] = line_group;
    items->count++;
    return true;
}

static void xargs_items_free(struct xargs_items *items) {
    if (!items)
        return;
    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    free(items->line_groups);
    items->v = NULL;
    items->line_groups = NULL;
    items->count = 0;
    items->cap = 0;
}

static bool xargs_buf_append(char **buf, size_t *len, size_t *cap, int ch) {
    if (*len + 1 >= *cap) {
        size_t new_cap = *cap == 0 ? 64 : *cap * 2;
        char *tmp = realloc(*buf, new_cap);
        if (!tmp)
            return false;
        *buf = tmp;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = (char)ch;
    (*buf)[*len] = '\0';
    return true;
}

static bool xargs_finalize_item(struct xargs_items *items, const char *buf, bool have_item,
                                const char *logical_eof, bool *stop, int line_group) {
    if (!have_item)
        return true;
    if (logical_eof && strcmp(buf ? buf : "", logical_eof) == 0) {
        if (stop)
            *stop = true;
        return true;
    }
    return xargs_items_append(items, buf ? buf : "", line_group);
}

static bool xargs_read_items_null(FILE *input, struct xargs_items *items) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int ch;

    while ((ch = fgetc(input)) != EOF) {
        if (ch == '\0') {
            if (!xargs_items_append(items, buf ? buf : "", items->count + 1)) {
                free(buf);
                return false;
            }
            free(buf);
            buf = NULL;
            len = 0;
            cap = 0;
            continue;
        }
        if (!xargs_buf_append(&buf, &len, &cap, ch)) {
            free(buf);
            return false;
        }
    }

    if (buf && len > 0) {
        if (!xargs_items_append(items, buf, items->count + 1)) {
            free(buf);
            return false;
        }
    }
    free(buf);
    return true;
}

static bool xargs_read_items_delim(FILE *input, struct xargs_items *items,
                                   char delimiter, const char *logical_eof) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    bool have_item = false;
    bool stop = false;
    int ch;

    while (!stop && (ch = fgetc(input)) != EOF) {
        if ((char)ch == delimiter) {
            if (!xargs_finalize_item(items, buf, true, logical_eof, &stop, items->count + 1)) {
                free(buf);
                return false;
            }
            len = 0;
            if (buf)
                buf[0] = '\0';
            have_item = false;
            continue;
        }

        if (!xargs_buf_append(&buf, &len, &cap, ch)) {
            free(buf);
            return false;
        }
        have_item = true;
    }

    if (!stop && have_item) {
        if (!xargs_finalize_item(items, buf, true, logical_eof, &stop, items->count + 1)) {
            free(buf);
            return false;
        }
    }

    free(buf);
    return true;
}

static bool xargs_read_items_default(FILE *input, const char *progname,
                                     struct xargs_items *items, const char *logical_eof) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int quote = 0;
    bool escaped = false;
    bool have_item = false;
    bool stop = false;
    int next_line_group = 1;
    int current_line_group = 0;
    int ch;

    while (!stop && (ch = fgetc(input)) != EOF) {
        if (quote == '\'') {
            if (ch == '\'') {
                quote = 0;
                have_item = true;
                continue;
            }
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (quote == '"') {
            if (escaped) {
                if (!xargs_buf_append(&buf, &len, &cap, ch))
                    goto oom;
                escaped = false;
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                have_item = true;
                continue;
            }
            if (ch == '\\') {
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                escaped = true;
                have_item = true;
                continue;
            }
            if (ch == '"') {
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                quote = 0;
                have_item = true;
                continue;
            }
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (escaped) {
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            escaped = false;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (ch == '\\') {
            if (current_line_group == 0)
                current_line_group = next_line_group;
            escaped = true;
            have_item = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (current_line_group == 0)
                current_line_group = next_line_group;
            quote = ch;
            have_item = true;
            continue;
        }
        if (isspace((unsigned char)ch)) {
            if (have_item) {
                if (!xargs_finalize_item(items, buf, true, logical_eof, &stop, current_line_group ? current_line_group : next_line_group))
                    goto oom;
                len = 0;
                if (buf)
                    buf[0] = '\0';
                have_item = false;
            }
            if (ch == '\n') {
                if (current_line_group != 0)
                    next_line_group = current_line_group + 1;
                current_line_group = 0;
            }
            continue;
        }

        if (!xargs_buf_append(&buf, &len, &cap, ch))
            goto oom;
        if (current_line_group == 0)
            current_line_group = next_line_group;
        have_item = true;
    }

    if (quote || escaped) {
        fprintf(stderr, "%s: unterminated quote or escape in input\n", progname);
        free(buf);
        return false;
    }

    if (!stop && have_item) {
        if (!xargs_finalize_item(items, buf, true, logical_eof, &stop, current_line_group ? current_line_group : next_line_group))
            goto oom;
    }

    free(buf);
    return true;

oom:
    free(buf);
    return false;
}

static bool xargs_read_items_replace_lines(FILE *input, struct xargs_items *items,
                                           const char *logical_eof) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_group = 1;

    while ((len = getline(&line, &cap, input)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (logical_eof && strcmp(line, logical_eof) == 0)
            break;
        bool blank = true;
        for (ssize_t i = 0; i < len; i++) {
            if (!isspace((unsigned char)line[i])) {
                blank = false;
                break;
            }
        }
        if (blank)
            continue;
        if (!xargs_items_append(items, line, line_group++)) {
            free(line);
            return false;
        }
    }

    free(line);
    return true;
}

static bool xargs_read_items(FILE *input, const char *progname, struct xargs_opts *opts,
                             struct xargs_items *items) {
    if (opts->replace_mode)
        return xargs_read_items_replace_lines(input, items, opts->logical_eof);
    if (opts->nul_delim)
        return xargs_read_items_null(input, items);
    if (opts->delimiter_mode)
        return xargs_read_items_delim(input, items, opts->delimiter, NULL);
    return xargs_read_items_default(input, progname, items, opts->logical_eof);
}

static void xargs_record_status(const char *progname, const char *cmdname, int status,
                                bool exec_failed, int *final_rc, bool *abort_launch) {
    int rc = 0;

    if (exec_failed) {
        rc = (status == ENOENT) ? 127 : 126;
        fprintf(stderr, "%s: failed to run command '%s': %s\n",
                progname, cmdname, strerror(status));
        *abort_launch = true;
    } else if (WIFSIGNALED(status)) {
        rc = 125;
        fprintf(stderr, "%s: %s: terminated by signal %d\n",
                progname, cmdname, WTERMSIG(status));
        *abort_launch = true;
    } else if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 255) {
            rc = 124;
            fprintf(stderr, "%s: %s: exited with status 255; aborting\n",
                    progname, cmdname);
            *abort_launch = true;
        } else if (exit_code != 0) {
            rc = 123;
        }
    } else {
        rc = 125;
        *abort_launch = true;
    }

    if (rc > *final_rc)
        *final_rc = rc;
}

static void xargs_fprint_argv(FILE *fp, char *const *argv) {
    for (int i = 0; argv[i]; i++)
        fprintf(fp, "%s%s", i == 0 ? "" : " ", argv[i]);
}

static int xargs_prompt_hook(const char *progname, char *const *argv, void *user) {
    (void)user;
    FILE *tty = fopen("/dev/tty", "r+");
    if (!tty) {
        fprintf(stderr, "%s: failed to open /dev/tty for reading: %s\n",
                progname, strerror(errno));
        return BX_CHILD_PROMPT_ERROR;
    }

    xargs_fprint_argv(tty, argv);
    fputs("?...", tty);
    fflush(tty);

    char *line = NULL;
    size_t cap = 0;
    ssize_t len = getline(&line, &cap, tty);
    fclose(tty);

    if (len < 0) {
        free(line);
        return BX_CHILD_PROMPT_ERROR;
    }

    bool approved = (len > 0 && (line[0] == 'y' || line[0] == 'Y'));
    free(line);
    return approved ? BX_CHILD_PROMPT_RUN : BX_CHILD_PROMPT_SKIP;
}

static void xargs_verbose_hook(const char *progname, char *const *argv, void *user) {
    (void)progname;
    (void)user;
    xargs_fprint_argv(stderr, argv);
    fputc('\n', stderr);
}

static char *xargs_expand_argument(const char *arg, const char *marker,
                                   const char *replacement, bool *saw_marker) {
    size_t arg_len = strlen(arg);
    size_t marker_len = strlen(marker);
    size_t replacement_len = strlen(replacement);
    size_t count = 0;

    const char *p = arg;
    while ((p = strstr(p, marker)) != NULL) {
        count++;
        p += marker_len;
    }

    if (count == 0)
        return strdup(arg);

    *saw_marker = true;
    size_t out_len = arg_len + count * replacement_len - count * marker_len + 1;
    char *out = malloc(out_len);
    if (!out)
        return NULL;

    char *dst = out;
    const char *src = arg;
    while ((p = strstr(src, marker)) != NULL) {
        size_t prefix = (size_t)(p - src);
        memcpy(dst, src, prefix);
        dst += prefix;
        memcpy(dst, replacement, replacement_len);
        dst += replacement_len;
        src = p + marker_len;
    }
    strcpy(dst, src);
    return out;
}

struct xargs_replacement_ctx {
    const char *marker;
};

static size_t xargs_replacement_marker_count(const char *arg, void *user) {
    const struct xargs_replacement_ctx *ctx = user;
    const char *marker = ctx ? ctx->marker : NULL;
    size_t count = 0;
    size_t marker_len = marker ? strlen(marker) : 0;

    if (!arg || marker_len == 0)
        return 0;

    const char *p = arg;
    while ((p = strstr(p, marker)) != NULL) {
        count++;
        p += marker_len;
    }

    return count;
}

static char *xargs_expand_replacement_arg(const char *arg, const char *item, void *user) {
    bool saw_marker = false;
    const struct xargs_replacement_ctx *ctx = user;
    const char *marker = ctx ? ctx->marker : NULL;
    return xargs_expand_argument(arg, marker, item, &saw_marker);
}

static size_t xargs_expand_replacement_bytes(const char *arg, const char *item, void *user) {
    const struct xargs_replacement_ctx *ctx = user;
    const char *marker = ctx ? ctx->marker : NULL;
    size_t marker_len = marker ? strlen(marker) : 0;
    size_t replacement_len = item ? strlen(item) : 0;
    size_t arg_bytes = strlen(arg) + 1;

    if (marker_len == 0)
        return arg_bytes;

    const char *p = arg;
    while ((p = strstr(p, marker)) != NULL) {
        arg_bytes += replacement_len;
        arg_bytes -= marker_len;
        p += marker_len;
    }

    return arg_bytes;
}

static int xargs_spawn_batch(const char *progname, char **command, int command_argc,
                             char **items, int item_count,
                             struct xargs_opts *opts,
                             int slot,
                             struct bx_child *children, int *running,
                             int *final_rc, bool *abort_launch) {
    char **argv = bx_argv_build_with_item_expansion((const char *const *)command, command_argc,
                                                    items, 0, item_count, 1,
                                                    NULL, NULL, NULL, NULL);
    if (!argv) {
        return 1;
    }
    struct bx_child_runner_opts runner_opts =
        bx_child_runner_opts_make(false,
                                  opts && opts->open_tty,
                                  opts ? opts->process_slot_var : NULL);
    if (opts && opts->interactive)
        runner_opts.prompt_hook = xargs_prompt_hook;
    if (opts && opts->verbose)
        runner_opts.verbose_hook = xargs_verbose_hook;
    bool exec_failed_now = false;
    int exec_errno_now = 0;
    int rc = bx_child_spawn_argv(progname, argv, &runner_opts, slot,
                                 children, running, &exec_failed_now, &exec_errno_now);
    if (exec_failed_now)
        xargs_record_status(progname, command[0], exec_errno_now, true, final_rc, abort_launch);
    bx_argv_free(argv);
    return rc;
}

static int xargs_spawn_replacement(const char *progname, char **command, int command_argc,
                                   const char *marker, const char *item,
                                   struct xargs_opts *opts,
                                   int slot,
                                   struct bx_child *children, int *running,
                                   int *final_rc, bool *abort_launch) {
    size_t char_limit = bx_argv_effective_char_limit(opts->max_chars);
    struct xargs_replacement_ctx expand_ctx = {
        .marker = marker,
    };
    char *item_argv[] = { NULL };
    item_argv[0] = strdup(item);
    if (!item_argv[0])
        return 1;
    if (char_limit > 0 &&
        bx_argv_bytes_with_item_expansion((const char *const *)command, command_argc,
                                          item_argv, 0, 1, 0,
                                          xargs_replacement_marker_count,
                                          xargs_expand_replacement_bytes,
                                          NULL, &expand_ctx) > char_limit) {
        free(item_argv[0]);
        fprintf(stderr, "%s: argument line too long\n", progname);
        return 1;
    }
    char **argv = bx_argv_build_with_item_expansion((const char *const *)command, command_argc,
                                                    item_argv, 0, 1, 0,
                                                    xargs_replacement_marker_count,
                                                    xargs_expand_replacement_arg,
                                                    NULL, &expand_ctx);
    free(item_argv[0]);
    if (!argv)
        return 1;

    if (char_limit > 0 && bx_argv_bytes(argv) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        bx_argv_free(argv);
        return 1;
    }

    struct bx_child_runner_opts runner_opts =
        bx_child_runner_opts_make(false,
                                  opts && opts->open_tty,
                                  opts ? opts->process_slot_var : NULL);
    if (opts && opts->interactive)
        runner_opts.prompt_hook = xargs_prompt_hook;
    if (opts && opts->verbose)
        runner_opts.verbose_hook = xargs_verbose_hook;
    bool exec_failed_now = false;
    int exec_errno_now = 0;
    int rc = bx_child_spawn_argv(progname, argv, &runner_opts, slot,
                                 children, running, &exec_failed_now, &exec_errno_now);
    if (exec_failed_now)
        xargs_record_status(progname, command[0], exec_errno_now, true, final_rc, abort_launch);
    bx_argv_free(argv);
    return rc;
}

struct xargs_reap_ctx {
    const char *progname;
    const char *cmdname;
    int *final_rc;
    bool *abort_launch;
};

static void xargs_reap_status_cb(pid_t pid, int status, bool exec_failed, int exec_errno, void *user) {
    (void)pid;
    struct xargs_reap_ctx *ctx = user;
    if (exec_failed)
        return;
    xargs_record_status(ctx->progname, ctx->cmdname, exec_failed ? exec_errno : status,
                        exec_failed, ctx->final_rc, ctx->abort_launch);
}

static int xargs_reap_children(const char *progname, const char *cmdname,
                               struct bx_child *children, int *running,
                               int *final_rc, bool *abort_launch,
                               bool block, bool drain_all) {
    struct xargs_reap_ctx ctx = {
        .progname = progname,
        .cmdname = cmdname,
        .final_rc = final_rc,
        .abort_launch = abort_launch,
    };
    return bx_child_reap(children, running, block, drain_all, xargs_reap_status_cb, &ctx);
}

static int xargs_wait_for_running_children(const char *progname, const char *cmdname,
                                           struct bx_child *children, int *running,
                                           int *final_rc, bool *abort_launch) {
    while (*running > 0) {
        if (xargs_reap_children(progname, cmdname, children, running,
                                final_rc, abort_launch, true, true) != 0)
            return 1;
    }
    return 0;
}

static int xargs_run_batches(const char *progname, char **command, int command_argc,
                             struct xargs_items *items, struct xargs_opts *opts) {
    if (opts->replace_mode && items->count == 0)
        return 0;

    if (items->count == 0 && opts->no_run_if_empty)
        return 0;

    int max_procs = opts->max_procs > 0 ? opts->max_procs : (items->count > 0 ? items->count : 1);
    struct bx_child *children = calloc((size_t)max_procs, sizeof(*children));
    if (!children)
        return 1;

    int final_rc = 0;
    bool abort_launch = false;
    int running = 0;

    size_t char_limit = bx_argv_effective_char_limit(opts->max_chars);
    if (char_limit > 0 &&
        bx_argv_bytes_with_items((const char *const *)command, command_argc, items->v, 0, 0) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        free(children);
        return 1;
    }

    if (items->count == 0) {
        int slot = bx_child_pick_slot(children, running, max_procs);
        if (xargs_spawn_batch(progname, command, command_argc, NULL, 0, opts, slot,
                              children, &running, &final_rc, &abort_launch) != 0) {
            (void)xargs_wait_for_running_children(progname, command[0], children, &running,
                                                  &final_rc, &abort_launch);
            free(children);
            return 1;
        }
    }

    for (int i = 0; i < items->count && !abort_launch; ) {
        while (running >= max_procs) {
            if (xargs_reap_children(progname, command[0], children, &running,
                                    &final_rc, &abort_launch, true, false) != 0) {
                free(children);
                return 1;
            }
        }
        if (abort_launch)
            break;

        int take = opts->replace_mode ? 1 : bx_argv_select_batch_count((const char *const *)command,
                                                                       command_argc,
                                                                       items->v, items->line_groups,
                                                                       items->count, i,
                                                                       opts->max_args, opts->max_lines,
                                                                       bx_argv_effective_char_limit(opts->max_chars));
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            (void)xargs_wait_for_running_children(progname, command[0], children, &running,
                                                  &final_rc, &abort_launch);
            free(children);
            return 1;
        }

        int slot = bx_child_pick_slot(children, running, max_procs);
        int spawn_rc;
        if (opts->replace_mode) {
            spawn_rc = xargs_spawn_replacement(progname, command, command_argc,
                                               opts->replace_marker ? opts->replace_marker : "{}",
                                               items->v[i], opts, slot, children, &running,
                                               &final_rc, &abort_launch);
        } else {
            spawn_rc = xargs_spawn_batch(progname, command, command_argc, &items->v[i], take, opts, slot,
                                         children, &running, &final_rc, &abort_launch);
        }
        if (spawn_rc != 0) {
            (void)xargs_wait_for_running_children(progname, command[0], children, &running,
                                                  &final_rc, &abort_launch);
            free(children);
            return 1;
        }
        i += take;

        if (running > 0) {
            if (xargs_reap_children(progname, command[0], children, &running,
                                    &final_rc, &abort_launch, false, true) != 0) {
                free(children);
                return 1;
            }
        }
    }

    while (running > 0) {
        if (xargs_wait_for_running_children(progname, command[0], children, &running,
                                            &final_rc, &abort_launch) != 0) {
            free(children);
            return 1;
        }
    }

    free(children);
    return final_rc;
}

int bx_xargs_main(int argc, char **argv) {
    const char *progname = argv[0] ? argv[0] : "xargs";
    struct xargs_opts opts = {
        .max_procs = 1,
    };

    static struct option long_opts[] = {
        {"help", no_argument, NULL, 200},
        {"version", no_argument, NULL, 201},
        {"show-limits", no_argument, NULL, 202},
        {"eof", optional_argument, NULL, 203},
        {"null", no_argument, NULL, '0'},
        {"delimiter", required_argument, NULL, 'd'},
        {"exit", no_argument, NULL, 'x'},
        {"no-run-if-empty", no_argument, NULL, 'r'},
        {"max-args", required_argument, NULL, 'n'},
        {"max-lines", required_argument, NULL, 'L'},
        {"max-chars", required_argument, NULL, 's'},
        {"open-tty", no_argument, NULL, 'o'},
        {"interactive", no_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 't'},
        {"replace", optional_argument, NULL, 'i'},
        {"process-slot-var", required_argument, NULL, 204},
        {"max-procs", required_argument, NULL, 'P'},
        {"arg-file", required_argument, NULL, 'a'},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    optind = 1;

    int c;
    while ((c = getopt_long(argc, argv, "+0d:E:e::I:i::l::L:oprs:txa:n:P:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'r':
            opts.no_run_if_empty = true;
            break;
        case 'o':
            opts.open_tty = true;
            break;
        case 'p':
            opts.interactive = true;
            break;
        case 't':
            opts.verbose = true;
            break;
        case 'x':
            opts.exit_if_too_big = true;
            break;
        case '0':
            opts.nul_delim = true;
            break;
        case 'd':
            if (!xargs_parse_delimiter(progname, optarg, &opts.delimiter))
                return 1;
            opts.delimiter_mode = true;
            break;
        case 'I':
            xargs_set_replace_mode(&opts, progname, optarg);
            break;
        case 'i':
            xargs_set_replace_mode(&opts, progname, optarg ? optarg : "{}");
            break;
        case 'E':
            opts.logical_eof = optarg;
            break;
        case 'e':
            opts.logical_eof = optarg ? optarg : NULL;
            break;
        case 'a':
            opts.arg_file = optarg;
            break;
        case 'n':
            if (!xargs_parse_int(progname, "-n", optarg, &opts.max_args, false))
                return 1;
            xargs_set_max_args(&opts, progname, opts.max_args);
            break;
        case 's':
            if (!xargs_parse_int(progname, "-s", optarg, &opts.max_chars, false))
                return 1;
            break;
        case 'L':
            if (!xargs_parse_int(progname, "-L", optarg, &opts.max_lines, false))
                return 1;
            xargs_set_max_lines(&opts, progname, "-L", opts.max_lines);
            break;
        case 'l':
            if (optarg) {
                if (!xargs_parse_int(progname, "-l", optarg, &opts.max_lines, false))
                    return 1;
                xargs_set_max_lines(&opts, progname, "-l", opts.max_lines);
            } else {
                xargs_set_max_lines(&opts, progname, "-l", 1);
            }
            break;
        case 'P':
            if (!xargs_parse_int(progname, "-P", optarg, &opts.max_procs, true))
                return 1;
            break;
        case 200:
            xargs_print_help(progname);
            return 0;
        case 201:
            xargs_print_version();
            return 0;
        case 202:
            xargs_print_limits();
            return 0;
        case 203:
            opts.logical_eof = optarg ? optarg : NULL;
            break;
        case 204:
            opts.process_slot_var = optarg;
            break;
        case '?':
            if (optopt == 'n' || optopt == 'L' || optopt == 'P' || optopt == 'a' || optopt == 'd' || optopt == 'E' || optopt == 'I' || optopt == 's') {
                fprintf(stderr, "%s: option requires an argument -- '-%c'\n", progname, optopt);
            } else if (optopt) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", progname, optopt);
            } else if (optind > 0 && optind <= argc) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", progname);
            }
            return 1;
        default:
            return 1;
        }
    }

    if (opts.nul_delim || opts.delimiter_mode)
        opts.logical_eof = NULL;
    if (opts.replace_mode && !opts.replace_marker)
        opts.replace_marker = "{}";

    char *default_command[] = { "echo", NULL };
    char **command = (optind < argc) ? &argv[optind] : default_command;
    int command_argc = (optind < argc) ? (argc - optind) : 1;

    FILE *input = stdin;
    if (opts.arg_file) {
        input = fopen(opts.arg_file, opts.nul_delim ? "rb" : "r");
        if (!input) {
            fprintf(stderr, "%s: %s: %s\n", progname, opts.arg_file, strerror(errno));
            return 1;
        }
    }

    struct xargs_items items = {0};
    if (!xargs_read_items(input, progname, &opts, &items)) {
        if (opts.arg_file)
            fclose(input);
        xargs_items_free(&items);
        return 1;
    }

    if (opts.arg_file)
        fclose(input);

    int rc = xargs_run_batches(progname, command, command_argc, &items, &opts);
    xargs_items_free(&items);
    return rc;
}
