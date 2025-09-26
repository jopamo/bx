#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include "fd_exec.h"
#include "lib/argv_packer.h"
#include "lib/child_runner.h"

static volatile sig_atomic_t fd_interrupt_signal = 0;

struct fd_signal_handlers {
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    bool has_int;
    bool has_term;
    bool has_hup;
};

static void fd_handle_interrupt_signal(int signo) {
    fd_interrupt_signal = signo;
}

static bool fd_exec_items_append_owned(struct fd_exec_items *items, char *text) {
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

bool fd_exec_items_append_path(struct fd_exec_items *items,
                               const struct fd_render_ctx *ctx,
                               const char *path) {
    char *exec_path = fd_render_exec_path(ctx, path);
    if (!exec_path)
        return false;
    if (!fd_exec_items_append_owned(items, exec_path)) {
        free(exec_path);
        return false;
    }
    return true;
}

void fd_exec_items_free(struct fd_exec_items *items) {
    if (!items)
        return;

    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    items->v = NULL;
    items->count = 0;
    items->cap = 0;
}

static int fd_install_one_signal_handler(int signo, struct sigaction *old_action) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fd_handle_interrupt_signal;
    sigemptyset(&sa.sa_mask);
    return sigaction(signo, &sa, old_action);
}

static int fd_install_signal_handlers(const char *progname,
                                      struct fd_signal_handlers *handlers) {
    memset(handlers, 0, sizeof(*handlers));
    fd_interrupt_signal = 0;

    if (fd_install_one_signal_handler(SIGINT, &handlers->old_int) != 0) {
        fprintf(stderr, "%s: cannot install SIGINT handler: %s\n", progname, strerror(errno));
        return 1;
    }
    handlers->has_int = true;

    if (fd_install_one_signal_handler(SIGTERM, &handlers->old_term) != 0) {
        fprintf(stderr, "%s: cannot install SIGTERM handler: %s\n", progname, strerror(errno));
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_int = false;
        return 1;
    }
    handlers->has_term = true;

    if (fd_install_one_signal_handler(SIGHUP, &handlers->old_hup) != 0) {
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

static void fd_restore_signal_handlers(struct fd_signal_handlers *handlers) {
    if (handlers->has_hup)
        sigaction(SIGHUP, &handlers->old_hup, NULL);
    if (handlers->has_term)
        sigaction(SIGTERM, &handlers->old_term, NULL);
    if (handlers->has_int)
        sigaction(SIGINT, &handlers->old_int, NULL);
}

static size_t fd_exec_arg_marker_count(const char *arg, void *user) {
    (void)user;
    return fd_placeholder_count(arg);
}

static size_t fd_exec_arg_expanded_bytes(const char *arg, const char *item, void *user) {
    (void)user;
    char *expanded = fd_expand_placeholders(arg, item ? item : "");
    if (!expanded)
        return (size_t)-1;
    size_t bytes = strlen(expanded) + 1;
    free(expanded);
    return bytes;
}

struct fd_exec_batch_ctx {
    const char **command_argv;
    int command_argc;
    char **items;
};

static size_t fd_exec_batch_ctx_bytes(void *user, int start, int count) {
    struct fd_exec_batch_ctx *ctx = user;
    return bx_argv_bytes_with_item_expansion(ctx->command_argv, ctx->command_argc,
                                             ctx->items, start, count, true,
                                             fd_exec_arg_marker_count,
                                             fd_exec_arg_expanded_bytes,
                                             NULL, NULL);
}

static int fd_select_exec_batch_count(const char **command_argv, int command_argc,
                                      char **items, int item_count, int start,
                                      size_t char_limit) {
    bool saw_placeholder = false;
    for (int i = 0; i < command_argc; i++) {
        if (fd_placeholder_count(command_argv[i]) > 0) {
            saw_placeholder = true;
            break;
        }
    }

    if (!saw_placeholder) {
        return bx_argv_select_batch_count(command_argv, command_argc,
                                          items, NULL, item_count, start,
                                          0, 0, char_limit);
    }

    struct fd_exec_batch_ctx ctx = {
        .command_argv = command_argv,
        .command_argc = command_argc,
        .items = items,
    };
    return bx_argv_select_batch_count_by_bytes(item_count, start, 0, 0, char_limit,
                                               fd_exec_batch_ctx_bytes, &ctx);
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

int fd_count_placeholder_args(const struct fd_opts *opts) {
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

static int fd_finish_interrupted_exec(struct bx_child *child, int *running) {
    int signo = (int)fd_interrupt_signal;
    if (signo == 0)
        return 0;

    bx_child_signal_all(child, *running, signo);
    while (*running > 0) {
        if (bx_child_reap(child, running, true, true, NULL, NULL) != 0)
            return 1;
    }
    return 128 + signo;
}

static int fd_run_exec_commands_inner(const char *progname, const struct fd_opts *opts,
                                      struct fd_exec_items *items) {
    if (opts->exec_mode == FD_EXEC_NONE || items->count == 0)
        return 0;

    size_t char_limit = bx_argv_effective_char_limit(0);
    int final_rc = 0;
    int running = 0;
    struct bx_child child = {0};
    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();
    int i = 0;

    while (i < items->count) {
        if (fd_interrupt_signal != 0) {
            int rc = fd_finish_interrupted_exec(&child, &running);
            return rc != 0 ? rc : 1;
        }

        int take = 1;
        if (opts->exec_mode == FD_EXEC_BATCH) {
            take = fd_select_exec_batch_count(opts->exec_argv, opts->exec_argc,
                                              items->v, items->count, i,
                                              char_limit);
            if (take < 0) {
                fprintf(stderr, "%s: argument line too long\n", progname);
                return 1;
            }
            if (opts->batch_size > 0 && take > opts->batch_size)
                take = opts->batch_size;
        } else if (char_limit > 0) {
            size_t bytes;
            if (fd_count_placeholder_args(opts) == 0) {
                bytes = bx_argv_bytes_with_items(opts->exec_argv, opts->exec_argc,
                                                 items->v, i, 1);
            } else {
                bytes = bx_argv_bytes_with_item_expansion(opts->exec_argv,
                                                          opts->exec_argc,
                                                          items->v, i, 1, false,
                                                          fd_exec_arg_marker_count,
                                                          fd_exec_arg_expanded_bytes,
                                                          NULL, NULL);
                if (bytes == (size_t)-1)
                    return 1;
            }
            if (bytes > char_limit) {
                fprintf(stderr, "%s: argument line too long\n", progname);
                return 1;
            }
        }

        char **argv = fd_build_exec_argv(opts->exec_argv, opts->exec_argc,
                                         items->v, i, take,
                                         opts->exec_mode == FD_EXEC_BATCH);
        if (!argv)
            return 1;

        bool exec_failed_now = false;
        int exec_errno_now = 0;
        int spawn_rc = bx_child_spawn_argv(progname, argv, &runner_opts, 0,
                                           &child, &running,
                                           &exec_failed_now, &exec_errno_now);
        fd_free_exec_argv(argv);
        if (spawn_rc != 0) {
            if (fd_interrupt_signal != 0) {
                int rc = fd_finish_interrupted_exec(&child, &running);
                return rc != 0 ? rc : 1;
            }
            return 1;
        }
        if (exec_failed_now)
            final_rc = 1;

        struct fd_exec_reap_ctx ctx = {
            .progname = progname,
            .cmdname = opts->exec_argv[0],
            .final_rc = &final_rc,
        };
        if (bx_child_reap(&child, &running, true, true, fd_exec_reap_status_cb, &ctx) != 0)
            return 1;
        if (fd_interrupt_signal != 0) {
            int rc = fd_finish_interrupted_exec(&child, &running);
            return rc != 0 ? rc : 1;
        }

        if (exec_failed_now)
            break;
        i += take;
    }

    return final_rc;
}

int fd_run_exec_commands(const char *progname, const struct fd_opts *opts,
                         struct fd_exec_items *items) {
    struct fd_signal_handlers handlers;
    if (fd_install_signal_handlers(progname, &handlers) != 0)
        return 1;
    int rc = fd_run_exec_commands_inner(progname, opts, items);
    fd_restore_signal_handlers(&handlers);
    return rc;
}
