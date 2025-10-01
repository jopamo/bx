#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "find_exec.h"

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

static int find_install_one_signal_handler(int signo,
                                           struct sigaction *old_action) {
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
        fprintf(stderr, "%s: cannot install SIGINT handler: %s\n", progname,
                strerror(errno));
        return 1;
    }
    handlers->has_int = true;

    if (find_install_one_signal_handler(SIGTERM, &handlers->old_term) != 0) {
        fprintf(stderr, "%s: cannot install SIGTERM handler: %s\n", progname,
                strerror(errno));
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_int = false;
        return 1;
    }
    handlers->has_term = true;

    if (find_install_one_signal_handler(SIGHUP, &handlers->old_hup) != 0) {
        fprintf(stderr, "%s: cannot install SIGHUP handler: %s\n", progname,
                strerror(errno));
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

int find_interrupt_return_code(void) {
    return find_interrupt_signal != 0 ? 128 + (int)find_interrupt_signal : 0;
}

bool find_prompt_ok(const char *cmdname, const char *path) {
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

static size_t find_exec_placeholder_count(const char *arg, void *user) {
    (void)user;
    return (arg && strcmp(arg, "{}") == 0) ? 1u : 0u;
}

static size_t find_exec_expanded_bytes(const char *arg, const char *item,
                                       void *user) {
    (void)user;
    return strlen((arg && strcmp(arg, "{}") == 0) ? item : arg) + 1;
}

static char *find_exec_expand_arg(const char *arg, const char *item,
                                  void *user) {
    (void)user;
    return strdup((arg && strcmp(arg, "{}") == 0) ? item : arg);
}

struct find_exec_batch_ctx {
    struct find_expr *expr;
};

static size_t find_exec_batch_bytes(void *user, int start, int count) {
    struct find_exec_batch_ctx *ctx = user;
    return bx_argv_bytes_with_item_expansion(
        (const char *const *)ctx->expr->exec_argv, ctx->expr->exec_argc,
        ctx->expr->exec_items.v, start, count, 1, find_exec_placeholder_count,
        find_exec_expanded_bytes, NULL, NULL);
}

static int find_select_exec_batch_count(struct find_expr *expr, int start,
                                        size_t char_limit) {
    struct find_exec_batch_ctx ctx = {.expr = expr};
    return bx_argv_select_batch_count_by_bytes(expr->exec_items.count, start, 0,
                                               0, char_limit,
                                               find_exec_batch_bytes, &ctx);
}

bool find_execdir_split_path(const char *path, char **dir_out, char **arg_out) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    size_t dir_len = slash ? (size_t)(slash - path) : 0;

    char *dir = NULL;
    if (dir_len == 0)
        dir = strdup(".");
    else
        dir = strndup(path, dir_len);
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
                                         char **dir_out,
                                         int *group_count_out) {
    char *dir = NULL;
    char *first_arg = NULL;
    if (!find_execdir_split_path(expr->exec_items.v[start], &dir, &first_arg))
        return NULL;

    int group_count = 1;
    while (start + group_count < expr->exec_items.count) {
        char *next_dir = NULL;
        char *next_arg = NULL;
        if (!find_execdir_split_path(expr->exec_items.v[start + group_count],
                                     &next_dir, &next_arg)) {
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
        if (!find_execdir_split_path(expr->exec_items.v[start + i], &item_dir,
                                     &items[i])) {
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

static void find_exec_reap_status_cb(pid_t pid, int wait_status,
                                     bool exec_failed, int exec_errno,
                                     void *user) {
    (void)pid;
    struct find_exec_reap_ctx *ctx = user;
    if (exec_failed) {
        fprintf(stderr, "%s: failed to run command '%s': %s\n", ctx->progname,
                ctx->cmdname, strerror(exec_errno));
        *ctx->status = 1;
        return;
    }

    if ((WIFEXITED(wait_status) && WEXITSTATUS(wait_status) != 0) ||
        WIFSIGNALED(wait_status))
        *ctx->status = 1;
}

bool find_run_exec_one(struct find_state *st, struct find_expr *expr,
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

    char *items[] = {item};
    char **argv = bx_argv_build_with_item_expansion(
        (const char *const *)expr->exec_argv, expr->exec_argc, items, 0, 1, 0,
        find_exec_placeholder_count, find_exec_expand_arg, NULL, NULL);
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
                                       &child, &running, &exec_failed_now,
                                       &exec_errno_now);
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
    if (bx_child_reap(&child, &running, true, true, find_exec_reap_status_cb,
                      &ctx) != 0) {
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

    for (int i = 0; i < expr->exec_items.count;) {
        int take = find_select_exec_batch_count(expr, i, char_limit);
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            return 1;
        }

        char **argv = bx_argv_build_with_item_expansion(
            (const char *const *)expr->exec_argv, expr->exec_argc,
            expr->exec_items.v, i, take, 1, find_exec_placeholder_count,
            find_exec_expand_arg, NULL, NULL);
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
                                           &child, &running, &exec_failed_now,
                                           &exec_errno_now);
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
        if (bx_child_reap(&child, &running, true, true,
                          find_exec_reap_status_cb, &ctx) != 0) {
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
    if (!expr || expr->kind != FIND_EXPR_EXECDIR_PLUS ||
        expr->exec_items.count == 0)
        return 0;

    size_t char_limit = bx_argv_effective_char_limit(0);
    struct bx_child child = {0};
    int running = 0;
    int status = 0;

    for (int i = 0; i < expr->exec_items.count;) {
        char *cwd = NULL;
        int group_count = 0;
        char **group_items =
            find_execdir_collect_group(expr, i, &cwd, &group_count);
        if (!group_items || !cwd) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        int take = bx_argv_select_batch_count((const char *const *)expr->exec_argv,
                                              expr->exec_argc, group_items, NULL,
                                              group_count, 0, 0, 0, char_limit);
        if (take < 0) {
            fprintf(stderr, "%s: argument line too long\n", progname);
            free(cwd);
            find_execdir_free_split_items(group_items, group_count);
            return 1;
        }

        char **argv = bx_argv_build_with_item_expansion(
            (const char *const *)expr->exec_argv, expr->exec_argc, group_items,
            0, take, 1, find_exec_placeholder_count, find_exec_expand_arg, NULL,
            NULL);
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
                                           &child, &running, &exec_failed_now,
                                           &exec_errno_now);
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
        if (bx_child_reap(&child, &running, true, true,
                          find_exec_reap_status_cb, &ctx) != 0) {
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

int find_run_pending_exec_exprs(const char *progname, struct find_expr *expr) {
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
