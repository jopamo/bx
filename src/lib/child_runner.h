#ifndef BX_LIB_CHILD_RUNNER_H
#define BX_LIB_CHILD_RUNNER_H

#include <stdbool.h>
#include <sys/types.h>

struct bx_child_runner_opts {
    bool verbose;
    bool reopen_stdin_tty;
    const char *process_slot_var;
    const char *cwd;
};

static inline struct bx_child_runner_opts bx_child_runner_opts_default(void) {
    return (struct bx_child_runner_opts){0};
}

static inline struct bx_child_runner_opts
bx_child_runner_opts_make(bool verbose, bool reopen_stdin_tty,
                          const char *process_slot_var) {
    return (struct bx_child_runner_opts){
        .verbose = verbose,
        .reopen_stdin_tty = reopen_stdin_tty,
        .process_slot_var = process_slot_var,
        .cwd = NULL,
    };
}

struct bx_child {
    pid_t pid;
    bool exec_failed;
    int exec_errno;
    int slot;
};

int bx_child_pick_slot(struct bx_child *children, int count, int max_procs);
int bx_child_spawn_argv(const char *progname, char **argv,
                        const struct bx_child_runner_opts *opts,
                        int slot,
                        struct bx_child *children, int *running,
                        bool *exec_failed_now, int *exec_errno_now);
int bx_child_reap(struct bx_child *children, int *running,
                  bool block, bool drain_all,
                  void (*cb)(pid_t pid, int status, bool exec_failed, int exec_errno, void *user),
                  void *user);

#endif
