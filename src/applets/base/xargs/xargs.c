#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"

extern char **environ;

struct xargs_opts {
    bool no_run_if_empty;
    bool nul_delim;
    bool delimiter_mode;
    bool exit_if_too_big;
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

struct xargs_child {
    pid_t pid;
    bool exec_failed;
    int slot;
};

struct xargs_items {
    char **v;
    int *line_groups;
    int count;
    int cap;
};

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

    size_t env_bytes = 0;
    if (environ) {
        for (char **ep = environ; *ep; ep++)
            env_bytes += strlen(*ep) + 1;
    }

    printf("Your environment variables take up %zu bytes\n", env_bytes);
    printf("POSIX upper limit on argument length (this system): %ld\n", arg_max);
    if (arg_max > 0 && env_bytes < (size_t)arg_max)
        printf("Maximum command length we could try to use: %ld\n", arg_max - (long)env_bytes);
}

static size_t xargs_environment_bytes(void) {
    size_t env_bytes = 0;
    if (environ) {
        for (char **ep = environ; *ep; ep++)
            env_bytes += strlen(*ep) + 1;
    }
    return env_bytes;
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

static struct xargs_child *xargs_find_child(struct xargs_child *children, int count, pid_t pid) {
    for (int i = 0; i < count; i++) {
        if (children[i].pid == pid)
            return &children[i];
    }
    return NULL;
}

static int xargs_pick_slot(struct xargs_child *children, int count, int max_procs) {
    for (int slot = 0; slot < max_procs; slot++) {
        bool used = false;
        for (int i = 0; i < count; i++) {
            if (children[i].slot == slot) {
                used = true;
                break;
            }
        }
        if (!used)
            return slot;
    }
    return 0;
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

static int xargs_spawn_argv(const char *progname, char **command, char **argv,
                            struct xargs_opts *opts,
                            int slot,
                            struct xargs_child *children, int *running,
                            int *final_rc, bool *abort_launch) {
    int errpipe[2];
    if (pipe(errpipe) != 0) {
        fprintf(stderr, "%s: pipe failed: %s\n", progname, strerror(errno));
        return 1;
    }
    fcntl(errpipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(errpipe[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork failed: %s\n", progname, strerror(errno));
        close(errpipe[0]);
        close(errpipe[1]);
        return 1;
    }
    if (pid == 0) {
        int errnum;
        close(errpipe[0]);
        if (opts && opts->process_slot_var) {
            char slot_buf[32];
            snprintf(slot_buf, sizeof(slot_buf), "%d", slot);
            setenv(opts->process_slot_var, slot_buf, 1);
        }
        execvp(argv[0], argv);
        errnum = errno;
        (void)!write(errpipe[1], &errnum, sizeof(errnum));
        _exit(127);
    }

    if (opts && opts->verbose) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, "%s%s", i == 0 ? "" : " ", argv[i]);
        fputc('\n', stderr);
    }

    close(errpipe[1]);
    children[*running].pid = pid;
    children[*running].exec_failed = false;
    children[*running].slot = slot;
    (*running)++;

    int exec_errno = 0;
    ssize_t nread = read(errpipe[0], &exec_errno, sizeof(exec_errno));
    close(errpipe[0]);

    if (nread == (ssize_t)sizeof(exec_errno)) {
        children[*running - 1].exec_failed = true;
        xargs_record_status(progname, command[0], exec_errno, true, final_rc, abort_launch);
    }

    return 0;
}

static int xargs_spawn_batch(const char *progname, char **command, int command_argc,
                             char **items, int item_count,
                             struct xargs_opts *opts,
                             int slot,
                             struct xargs_child *children, int *running,
                             int *final_rc, bool *abort_launch) {
    char **argv = calloc((size_t)command_argc + (size_t)item_count + 1, sizeof(*argv));
    if (!argv) {
        return 1;
    }
    for (int j = 0; j < command_argc; j++)
        argv[j] = command[j];
    for (int j = 0; j < item_count; j++)
        argv[command_argc + j] = items[j];
    argv[command_argc + item_count] = NULL;
    int rc = xargs_spawn_argv(progname, command, argv, opts, slot,
                              children, running, final_rc, abort_launch);
    free(argv);
    return rc;
}

static int xargs_spawn_replacement(const char *progname, char **command, int command_argc,
                                   const char *marker, const char *item,
                                   struct xargs_opts *opts,
                                   int slot,
                                   struct xargs_child *children, int *running,
                                   int *final_rc, bool *abort_launch) {
    char **argv = calloc((size_t)command_argc + 1, sizeof(*argv));
    if (!argv)
        return 1;

    bool saw_marker = false;
    for (int i = 0; i < command_argc; i++) {
        argv[i] = xargs_expand_argument(command[i], marker, item, &saw_marker);
        if (!argv[i]) {
            for (int j = 0; j < i; j++)
                free(argv[j]);
            free(argv);
            return 1;
        }
    }
    argv[command_argc] = NULL;

    int rc = xargs_spawn_argv(progname, command, argv, opts, slot,
                              children, running, final_rc, abort_launch);
    for (int i = 0; i < command_argc; i++)
        free(argv[i]);
    free(argv);
    return rc;
}

static int xargs_reap_children(const char *progname, const char *cmdname,
                               struct xargs_child *children, int *running,
                               int *final_rc, bool *abort_launch,
                               bool block, bool drain_all) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, block ? 0 : WNOHANG);
        if (pid == 0)
            return 0;
        if (pid < 0) {
            if (!block && errno == ECHILD)
                return 0;
            return (errno == ECHILD) ? 0 : 1;
        }

        struct xargs_child *child = xargs_find_child(children, *running, pid);
        bool exec_failed = child && child->exec_failed;
        if (child) {
            *child = children[*running - 1];
            (*running)--;
        }

        if (!exec_failed)
            xargs_record_status(progname, cmdname, status, exec_failed, final_rc, abort_launch);
        if (!drain_all)
            return 0;
        if (*running == 0)
            return 0;
        block = false;
    }
}

static size_t xargs_argv_bytes(char **command, int command_argc, struct xargs_items *items,
                               int start, int count) {
    size_t total = 0;
    for (int i = 0; i < command_argc; i++)
        total += strlen(command[i]) + 1;
    for (int i = 0; i < count; i++)
        total += strlen(items->v[start + i]) + 1;
    return total;
}

static size_t xargs_effective_char_limit(struct xargs_opts *opts) {
    long arg_max = sysconf(_SC_ARG_MAX);
    size_t sys_limit = 0;
    if (arg_max > 0) {
        size_t env_bytes = xargs_environment_bytes();
        if ((size_t)arg_max > env_bytes)
            sys_limit = (size_t)arg_max - env_bytes;
    }

    if (opts->max_chars > 0 && sys_limit > 0)
        return (size_t)opts->max_chars < sys_limit ? (size_t)opts->max_chars : sys_limit;
    if (opts->max_chars > 0)
        return (size_t)opts->max_chars;
    if (sys_limit > 0)
        return sys_limit;
    return 0;
}

static int xargs_select_batch_count(char **command, int command_argc,
                                    struct xargs_items *items, int start,
                                    struct xargs_opts *opts) {
    int max_args = opts->max_args > 0 ? opts->max_args : (items->count - start);
    int max_lines = opts->max_lines > 0 ? opts->max_lines : (items->count - start);
    int take = 0;
    int used_lines = 0;
    int last_group = -1;
    size_t char_limit = xargs_effective_char_limit(opts);
    size_t bytes = xargs_argv_bytes(command, command_argc, items, start, 0);

    if (char_limit > 0 && bytes > char_limit)
        return -1;

    while (start + take < items->count && take < max_args) {
        int group = items->line_groups[start + take];
        if (take == 0 || group != last_group) {
            if (used_lines >= max_lines)
                break;
            used_lines++;
            last_group = group;
        }
        if (char_limit > 0) {
            size_t next_bytes = bytes + strlen(items->v[start + take]) + 1;
            if (next_bytes > char_limit)
                break;
            bytes = next_bytes;
        }
        take++;
    }

    return take > 0 ? take : -1;
}

static int xargs_run_batches(const char *progname, char **command, int command_argc,
                             struct xargs_items *items, struct xargs_opts *opts) {
    if (items->count == 0 && opts->no_run_if_empty)
        return 0;

    int max_procs = opts->max_procs > 0 ? opts->max_procs : 1;
    struct xargs_child *children = calloc((size_t)max_procs, sizeof(*children));
    if (!children)
        return 1;

    int final_rc = 0;
    bool abort_launch = false;
    int running = 0;

    if (xargs_effective_char_limit(opts) > 0 &&
        xargs_argv_bytes(command, command_argc, items, 0, 0) > xargs_effective_char_limit(opts)) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        free(children);
        return 1;
    }

    if (items->count == 0) {
        int slot = xargs_pick_slot(children, running, max_procs);
        if (xargs_spawn_batch(progname, command, command_argc, NULL, 0, opts, slot,
                              children, &running, &final_rc, &abort_launch) != 0) {
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

        int take = opts->replace_mode ? 1 : xargs_select_batch_count(command, command_argc, items, i, opts);
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            free(children);
            return 1;
        }

        int slot = xargs_pick_slot(children, running, max_procs);
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
        if (xargs_reap_children(progname, command[0], children, &running,
                                &final_rc, &abort_launch, true, true) != 0) {
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
    while ((c = getopt_long(argc, argv, "+0d:E:e::I:i::l::L:rs:txa:n:P:", long_opts, NULL)) != -1) {
        switch (c) {
        case 'r':
            opts.no_run_if_empty = true;
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
            opts.replace_mode = true;
            opts.replace_marker = optarg;
            break;
        case 'i':
            opts.replace_mode = true;
            opts.replace_marker = optarg ? optarg : "{}";
            break;
        case 'E':
            opts.logical_eof = optarg;
            break;
        case 'e':
            opts.logical_eof = optarg ? optarg : "_";
            break;
        case 'a':
            opts.arg_file = optarg;
            break;
        case 'n':
            if (!xargs_parse_int(progname, "-n", optarg, &opts.max_args, false))
                return 1;
            break;
        case 's':
            if (!xargs_parse_int(progname, "-s", optarg, &opts.max_chars, false))
                return 1;
            break;
        case 'L':
            if (!xargs_parse_int(progname, "-L", optarg, &opts.max_lines, false))
                return 1;
            break;
        case 'l':
            if (optarg) {
                if (!xargs_parse_int(progname, "-l", optarg, &opts.max_lines, false))
                    return 1;
            } else {
                opts.max_lines = 1;
            }
            break;
        case 'P':
            if (!xargs_parse_int(progname, "-P", optarg, &opts.max_procs, true))
                return 1;
            if (opts.max_procs == 0)
                opts.max_procs = 1;
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
            opts.logical_eof = optarg ? optarg : "_";
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
