#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "xargs_exec.h"

static void xargs_record_status(const char *progname, const char *cmdname,
                                int status, bool exec_failed, int *final_rc,
                                bool *abort_launch) {
    int rc = 0;

    if (exec_failed) {
        rc = (status == ENOENT) ? 127 : 126;
        fprintf(stderr, "%s: failed to run command '%s': %s\n", progname,
                cmdname, strerror(status));
        *abort_launch = true;
    } else if (WIFSIGNALED(status)) {
        rc = 125;
        fprintf(stderr, "%s: %s: terminated by signal %d\n", progname,
                cmdname, WTERMSIG(status));
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

static int xargs_prompt_hook(const char *progname, char *const *argv,
                             void *user) {
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

static void xargs_verbose_hook(const char *progname, char *const *argv,
                               void *user) {
    (void)progname;
    (void)user;
    xargs_fprint_argv(stderr, argv);
    fputc('\n', stderr);
}

static char *xargs_expand_argument(const char *arg, const char *marker,
                                   const char *replacement,
                                   bool *saw_marker) {
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
    size_t out_len =
        arg_len + count * replacement_len - count * marker_len + 1;
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

static char *xargs_expand_replacement_arg(const char *arg, const char *item,
                                          void *user) {
    bool saw_marker = false;
    const struct xargs_replacement_ctx *ctx = user;
    const char *marker = ctx ? ctx->marker : NULL;
    return xargs_expand_argument(arg, marker, item, &saw_marker);
}

static size_t xargs_expand_replacement_bytes(const char *arg, const char *item,
                                             void *user) {
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

static int xargs_spawn_batch(const char *progname, char **command,
                             int command_argc, char **items, int item_count,
                             struct xargs_opts *opts, int slot,
                             struct bx_child *children, int *running,
                             int *final_rc, bool *abort_launch) {
    char **argv = bx_argv_build_with_item_expansion(
        (const char *const *)command, command_argc, items, 0, item_count, 1,
        NULL, NULL, NULL, NULL);
    if (!argv)
        return 1;

    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_make(
        false, opts && opts->open_tty,
        opts ? opts->process_slot_var : NULL);
    if (opts && opts->interactive)
        runner_opts.prompt_hook = xargs_prompt_hook;
    if (opts && opts->verbose)
        runner_opts.verbose_hook = xargs_verbose_hook;

    bool exec_failed_now = false;
    int exec_errno_now = 0;
    int rc = bx_child_spawn_argv(progname, argv, &runner_opts, slot, children,
                                 running, &exec_failed_now, &exec_errno_now);
    if (exec_failed_now)
        xargs_record_status(progname, command[0], exec_errno_now, true,
                            final_rc, abort_launch);
    bx_argv_free(argv);
    return rc;
}

static int xargs_spawn_replacement(const char *progname, char **command,
                                   int command_argc, const char *marker,
                                   const char *item, struct xargs_opts *opts,
                                   int slot, struct bx_child *children,
                                   int *running, int *final_rc,
                                   bool *abort_launch) {
    size_t char_limit = bx_argv_effective_char_limit(opts->max_chars);
    struct xargs_replacement_ctx expand_ctx = {.marker = marker};
    char *item_argv[] = {NULL};
    item_argv[0] = strdup(item);
    if (!item_argv[0])
        return 1;
    if (char_limit > 0 &&
        bx_argv_bytes_with_item_expansion(
            (const char *const *)command, command_argc, item_argv, 0, 1, 0,
            xargs_replacement_marker_count, xargs_expand_replacement_bytes,
            NULL, &expand_ctx) > char_limit) {
        free(item_argv[0]);
        fprintf(stderr, "%s: argument line too long\n", progname);
        return 1;
    }
    char **argv = bx_argv_build_with_item_expansion(
        (const char *const *)command, command_argc, item_argv, 0, 1, 0,
        xargs_replacement_marker_count, xargs_expand_replacement_arg, NULL,
        &expand_ctx);
    free(item_argv[0]);
    if (!argv)
        return 1;

    if (char_limit > 0 && bx_argv_bytes(argv) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        bx_argv_free(argv);
        return 1;
    }

    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_make(
        false, opts && opts->open_tty,
        opts ? opts->process_slot_var : NULL);
    if (opts && opts->interactive)
        runner_opts.prompt_hook = xargs_prompt_hook;
    if (opts && opts->verbose)
        runner_opts.verbose_hook = xargs_verbose_hook;

    bool exec_failed_now = false;
    int exec_errno_now = 0;
    int rc = bx_child_spawn_argv(progname, argv, &runner_opts, slot, children,
                                 running, &exec_failed_now, &exec_errno_now);
    if (exec_failed_now)
        xargs_record_status(progname, command[0], exec_errno_now, true,
                            final_rc, abort_launch);
    bx_argv_free(argv);
    return rc;
}

struct xargs_reap_ctx {
    const char *progname;
    const char *cmdname;
    int *final_rc;
    bool *abort_launch;
};

static void xargs_reap_status_cb(pid_t pid, int status, bool exec_failed,
                                 int exec_errno, void *user) {
    (void)pid;
    struct xargs_reap_ctx *ctx = user;
    if (exec_failed)
        return;
    xargs_record_status(ctx->progname, ctx->cmdname,
                        exec_failed ? exec_errno : status, exec_failed,
                        ctx->final_rc, ctx->abort_launch);
}

static int xargs_reap_children(const char *progname, const char *cmdname,
                               struct bx_child *children, int *running,
                               int *final_rc, bool *abort_launch, bool block,
                               bool drain_all) {
    struct xargs_reap_ctx ctx = {
        .progname = progname,
        .cmdname = cmdname,
        .final_rc = final_rc,
        .abort_launch = abort_launch,
    };
    return bx_child_reap(children, running, block, drain_all,
                         xargs_reap_status_cb, &ctx);
}

static int xargs_finish_interrupted_run(volatile sig_atomic_t *interrupt_signal,
                                        struct bx_child *children,
                                        int *running) {
    int signo = (int)*interrupt_signal;
    if (signo == 0)
        return 0;

    bx_child_signal_all(children, *running, signo);
    while (*running > 0) {
        if (bx_child_reap(children, running, true, true, NULL, NULL) != 0)
            return 1;
    }
    return 128 + signo;
}

struct xargs_stream_batch {
    char **items;
    int *line_groups;
    int count;
    int cap;
    int used_lines;
    int last_group;
};

struct xargs_stream_ctx {
    const char *progname;
    char **command;
    int command_argc;
    struct xargs_opts *opts;
    struct bx_child *children;
    int max_procs;
    int *running;
    int *final_rc;
    bool *abort_launch;
    volatile sig_atomic_t *interrupt_signal;
    struct xargs_stream_batch batch;
    bool saw_item;
    bool failed;
};

static bool xargs_stream_grow_children(struct xargs_stream_ctx *ctx) {
    int new_max_procs = ctx->max_procs < 4 ? 4 : ctx->max_procs * 2;
    struct bx_child *new_children = realloc(
        ctx->children, (size_t)new_max_procs * sizeof(*new_children));
    if (!new_children) {
        fprintf(stderr, "%s: out of memory\n", ctx->progname);
        ctx->failed = true;
        return false;
    }

    memset(new_children + ctx->max_procs, 0,
           (size_t)(new_max_procs - ctx->max_procs) * sizeof(*new_children));
    ctx->children = new_children;
    ctx->max_procs = new_max_procs;
    return true;
}

static bool xargs_stream_wait_for_slot(struct xargs_stream_ctx *ctx) {
    while (*ctx->running >= ctx->max_procs) {
        if (ctx->opts->max_procs == 0) {
            return xargs_stream_grow_children(ctx);
        }

        if (xargs_reap_children(ctx->progname, ctx->command[0], ctx->children,
                                ctx->running, ctx->final_rc, ctx->abort_launch,
                                true, false) != 0) {
            ctx->failed = true;
            return false;
        }
        if (*ctx->interrupt_signal != 0 || *ctx->abort_launch)
            return false;
    }

    return true;
}

static void xargs_stream_batch_reset(struct xargs_stream_batch *batch) {
    for (int i = 0; i < batch->count; i++)
        free(batch->items[i]);
    batch->count = 0;
    batch->used_lines = 0;
    batch->last_group = -1;
}

static void xargs_stream_batch_free(struct xargs_stream_batch *batch) {
    xargs_stream_batch_reset(batch);
    free(batch->items);
    free(batch->line_groups);
    batch->items = NULL;
    batch->line_groups = NULL;
    batch->cap = 0;
}

static bool xargs_stream_batch_append(struct xargs_stream_batch *batch,
                                      const char *item, int line_group) {
    if (batch->count >= batch->cap) {
        int new_cap = batch->cap == 0 ? 16 : batch->cap * 2;
        char **new_items =
            realloc(batch->items, (size_t)new_cap * sizeof(*batch->items));
        if (!new_items)
            return false;
        batch->items = new_items;
        int *new_groups = realloc(batch->line_groups,
                                  (size_t)new_cap * sizeof(*batch->line_groups));
        if (!new_groups)
            return false;
        batch->line_groups = new_groups;
        batch->cap = new_cap;
    }

    batch->items[batch->count] = strdup(item);
    if (!batch->items[batch->count])
        return false;
    batch->line_groups[batch->count] = line_group;
    batch->count++;
    if (batch->used_lines == 0 || line_group != batch->last_group) {
        batch->used_lines++;
        batch->last_group = line_group;
    }
    return true;
}

static int xargs_stream_flush(struct xargs_stream_ctx *ctx) {
    if (ctx->batch.count == 0)
        return 0;

    if (!xargs_stream_wait_for_slot(ctx))
        return ctx->failed ? 1 : 0;

    int slot = bx_child_pick_slot(ctx->children, *ctx->running, ctx->max_procs);
    if (xargs_spawn_batch(ctx->progname, ctx->command, ctx->command_argc,
                          ctx->batch.items, ctx->batch.count, ctx->opts, slot,
                          ctx->children, ctx->running, ctx->final_rc,
                          ctx->abort_launch) != 0) {
        ctx->failed = true;
        return 1;
    }

    xargs_stream_batch_reset(&ctx->batch);

    if (*ctx->running > 0 &&
        xargs_reap_children(ctx->progname, ctx->command[0], ctx->children,
                            ctx->running, ctx->final_rc, ctx->abort_launch,
                            false, true) != 0) {
        ctx->failed = true;
        return 1;
    }

    return 0;
}

static bool xargs_stream_sink(const char *item, int line_group, void *user) {
    struct xargs_stream_ctx *ctx = user;
    struct xargs_stream_batch *batch = &ctx->batch;
    int max_args = ctx->opts->max_args > 0 ? ctx->opts->max_args : INT_MAX;
    int max_lines = ctx->opts->max_lines > 0 ? ctx->opts->max_lines : INT_MAX;
    size_t char_limit = bx_argv_effective_char_limit(ctx->opts->max_chars);

    ctx->saw_item = true;

    if (ctx->opts->replace_mode) {
        if (!xargs_stream_wait_for_slot(ctx))
            return false;

        int slot =
            bx_child_pick_slot(ctx->children, *ctx->running, ctx->max_procs);
        if (xargs_spawn_replacement(
                ctx->progname, ctx->command, ctx->command_argc,
                ctx->opts->replace_marker ? ctx->opts->replace_marker : "{}",
                item, ctx->opts, slot, ctx->children, ctx->running,
                ctx->final_rc, ctx->abort_launch) != 0) {
            ctx->failed = true;
            return false;
        }

        if (*ctx->running > 0 &&
            xargs_reap_children(ctx->progname, ctx->command[0], ctx->children,
                                ctx->running, ctx->final_rc,
                                ctx->abort_launch, false, true) != 0) {
            ctx->failed = true;
            return false;
        }
        return true;
    }

    bool new_group = (batch->count == 0 || line_group != batch->last_group);

    if (batch->count > 0 &&
        (batch->count >= max_args ||
         (new_group && batch->used_lines >= max_lines) ||
         (char_limit > 0 &&
          (bx_argv_bytes_with_items((const char *const *)ctx->command,
                                    ctx->command_argc, batch->items, 0,
                                    batch->count) +
           strlen(item) + 1 + sizeof(char *)) > char_limit))) {
        if (xargs_stream_flush(ctx) != 0)
            return false;
        if (*ctx->interrupt_signal != 0 || *ctx->abort_launch)
            return false;
        batch = &ctx->batch;
    }

    if (char_limit > 0) {
        size_t single_item_bytes =
            bx_argv_bytes_with_items((const char *const *)ctx->command,
                                     ctx->command_argc, NULL, 0, 0) +
            strlen(item) + 1 + sizeof(char *);
        if (single_item_bytes > char_limit) {
            fprintf(stderr, "%s: argument line too long\n", ctx->progname);
            ctx->failed = true;
            return false;
        }
    }

    if (!xargs_stream_batch_append(batch, item, line_group)) {
        ctx->failed = true;
        return false;
    }
    return true;
}

static int xargs_wait_for_running_children(
    volatile sig_atomic_t *interrupt_signal, const char *progname,
    const char *cmdname, struct bx_child *children, int *running,
    int *final_rc, bool *abort_launch) {
    while (*running > 0) {
        if (*interrupt_signal != 0)
            return 0;
        if (xargs_reap_children(progname, cmdname, children, running, final_rc,
                                abort_launch, true, true) != 0)
            return 1;
    }
    return 0;
}

int xargs_run_batches(const char *progname, char **command, int command_argc,
                      struct xargs_items *items, struct xargs_opts *opts,
                      volatile sig_atomic_t *interrupt_signal) {
    if (opts->replace_mode && items->count == 0)
        return 0;

    if (items->count == 0 && opts->no_run_if_empty)
        return 0;

    int max_procs =
        opts->max_procs > 0 ? opts->max_procs : (items->count > 0 ? items->count : 1);
    struct bx_child *children = calloc((size_t)max_procs, sizeof(*children));
    if (!children)
        return 1;

    int final_rc = 0;
    bool abort_launch = false;
    int running = 0;

    size_t char_limit = bx_argv_effective_char_limit(opts->max_chars);
    if (char_limit > 0 &&
        bx_argv_bytes_with_items((const char *const *)command, command_argc,
                                 items->v, 0, 0) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        free(children);
        return 1;
    }

    if (items->count == 0) {
        int slot = bx_child_pick_slot(children, running, max_procs);
        if (xargs_spawn_batch(progname, command, command_argc, NULL, 0, opts,
                              slot, children, &running, &final_rc,
                              &abort_launch) != 0) {
            (void)xargs_wait_for_running_children(interrupt_signal, progname,
                                                  command[0], children,
                                                  &running, &final_rc,
                                                  &abort_launch);
            if (*interrupt_signal != 0) {
                int rc = xargs_finish_interrupted_run(interrupt_signal,
                                                      children, &running);
                free(children);
                return rc != 0 ? rc : 1;
            }
            free(children);
            return 1;
        }
        if (*interrupt_signal != 0) {
            int rc =
                xargs_finish_interrupted_run(interrupt_signal, children, &running);
            free(children);
            return rc != 0 ? rc : 1;
        }
    }

    for (int i = 0; i < items->count && !abort_launch;) {
        if (*interrupt_signal != 0) {
            int rc =
                xargs_finish_interrupted_run(interrupt_signal, children, &running);
            free(children);
            return rc != 0 ? rc : 1;
        }
        while (running >= max_procs) {
            if (xargs_reap_children(progname, command[0], children, &running,
                                    &final_rc, &abort_launch, true,
                                    false) != 0) {
                free(children);
                return 1;
            }
            if (*interrupt_signal != 0) {
                int rc = xargs_finish_interrupted_run(interrupt_signal,
                                                      children, &running);
                free(children);
                return rc != 0 ? rc : 1;
            }
        }
        if (abort_launch)
            break;

        int take = opts->replace_mode
                       ? 1
                       : bx_argv_select_batch_count(
                             (const char *const *)command, command_argc,
                             items->v, items->line_groups, items->count, i,
                             opts->max_args, opts->max_lines,
                             bx_argv_effective_char_limit(opts->max_chars));
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            (void)xargs_wait_for_running_children(interrupt_signal, progname,
                                                  command[0], children,
                                                  &running, &final_rc,
                                                  &abort_launch);
            if (*interrupt_signal != 0) {
                int rc = xargs_finish_interrupted_run(interrupt_signal,
                                                      children, &running);
                free(children);
                return rc != 0 ? rc : 1;
            }
            free(children);
            return 1;
        }

        int slot = bx_child_pick_slot(children, running, max_procs);
        int spawn_rc;
        if (opts->replace_mode) {
            spawn_rc = xargs_spawn_replacement(
                progname, command, command_argc,
                opts->replace_marker ? opts->replace_marker : "{}", items->v[i],
                opts, slot, children, &running, &final_rc, &abort_launch);
        } else {
            spawn_rc = xargs_spawn_batch(progname, command, command_argc,
                                         &items->v[i], take, opts, slot,
                                         children, &running, &final_rc,
                                         &abort_launch);
        }
        if (spawn_rc != 0) {
            (void)xargs_wait_for_running_children(interrupt_signal, progname,
                                                  command[0], children,
                                                  &running, &final_rc,
                                                  &abort_launch);
            if (*interrupt_signal != 0) {
                int rc = xargs_finish_interrupted_run(interrupt_signal,
                                                      children, &running);
                free(children);
                return rc != 0 ? rc : 1;
            }
            free(children);
            return 1;
        }
        i += take;

        if (*interrupt_signal != 0) {
            int rc =
                xargs_finish_interrupted_run(interrupt_signal, children, &running);
            free(children);
            return rc != 0 ? rc : 1;
        }

        if (running > 0) {
            if (xargs_reap_children(progname, command[0], children, &running,
                                    &final_rc, &abort_launch, false,
                                    true) != 0) {
                free(children);
                return 1;
            }
            if (*interrupt_signal != 0) {
                int rc = xargs_finish_interrupted_run(interrupt_signal,
                                                      children, &running);
                free(children);
                return rc != 0 ? rc : 1;
            }
        }
    }

    while (running > 0) {
        if (xargs_wait_for_running_children(interrupt_signal, progname,
                                            command[0], children, &running,
                                            &final_rc, &abort_launch) != 0) {
            free(children);
            return 1;
        }
        if (*interrupt_signal != 0) {
            int rc =
                xargs_finish_interrupted_run(interrupt_signal, children, &running);
            free(children);
            return rc != 0 ? rc : 1;
        }
    }

    free(children);
    return final_rc;
}

int xargs_run_streaming_batches(const char *progname, char **command,
                                int command_argc, FILE *input,
                                struct xargs_opts *opts,
                                volatile sig_atomic_t *interrupt_signal) {
    int initial_max_procs = opts->max_procs > 0 ? opts->max_procs : 1;
    struct bx_child *children =
        calloc((size_t)initial_max_procs, sizeof(*children));
    if (!children)
        return 1;

    size_t char_limit = bx_argv_effective_char_limit(opts->max_chars);
    if (char_limit > 0 &&
        bx_argv_bytes_with_items((const char *const *)command, command_argc,
                                 NULL, 0, 0) > char_limit) {
        fprintf(stderr, "%s: argument line too long\n", progname);
        free(children);
        return 1;
    }

    int running = 0;
    int final_rc = 0;
    bool abort_launch = false;
    struct xargs_stream_ctx ctx = {
        .progname = progname,
        .command = command,
        .command_argc = command_argc,
        .opts = opts,
        .children = children,
        .max_procs = initial_max_procs,
        .running = &running,
        .final_rc = &final_rc,
        .abort_launch = &abort_launch,
        .interrupt_signal = interrupt_signal,
        .batch = {.last_group = -1},
    };

    bool read_ok = xargs_read_stream(input, progname, opts, xargs_stream_sink, &ctx);
    if (!read_ok && !ctx.failed &&
        (abort_launch || *interrupt_signal != 0))
        read_ok = true;
    if (read_ok && ctx.batch.count > 0 && !abort_launch && *interrupt_signal == 0 &&
        xargs_stream_flush(&ctx) != 0)
        read_ok = false;

    if (read_ok && !ctx.saw_item && !opts->no_run_if_empty &&
        !opts->replace_mode) {
        int slot =
            bx_child_pick_slot(ctx.children, running, ctx.max_procs);
        if (xargs_spawn_batch(progname, command, command_argc, NULL, 0, opts,
                              slot, ctx.children, &running, &final_rc,
                              &abort_launch) != 0)
            read_ok = false;
    }

    if (read_ok &&
        xargs_wait_for_running_children(interrupt_signal, progname, command[0],
                                        ctx.children, &running, &final_rc,
                                        &abort_launch) != 0)
        read_ok = false;

    if (*interrupt_signal != 0) {
        int rc =
            xargs_finish_interrupted_run(interrupt_signal, ctx.children, &running);
        xargs_stream_batch_free(&ctx.batch);
        free(ctx.children);
        return rc != 0 ? rc : 1;
    }

    xargs_stream_batch_free(&ctx.batch);
    free(ctx.children);
    return (read_ok && !ctx.failed) ? final_rc : 1;
}
