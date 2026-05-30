#ifndef BX_LIB_CHILD_RUNNER_H
#define BX_LIB_CHILD_RUNNER_H

#include <stdbool.h>
#include <signal.h>
#include <sys/types.h>

enum bx_child_prompt_result {
    BX_CHILD_PROMPT_ERROR = -1,
    BX_CHILD_PROMPT_SKIP = 0,
    BX_CHILD_PROMPT_RUN = 1,
};

typedef int (*bx_child_prompt_hook)(const char *progname, char *const *argv, void *user);
typedef void (*bx_child_verbose_hook)(const char *progname, char *const *argv, void *user);

struct bx_child_runner_opts {
    bool verbose;
    bool reopen_stdin_tty;
    const char *process_slot_var;
    const char *cwd;
    bool use_stdin_fd;
    int stdin_fd;
    bool use_stdout_fd;
    int stdout_fd;
    bool use_stderr_fd;
    int stderr_fd;
    bool reset_common_signals;
    bool new_process_group;
    bx_child_prompt_hook prompt_hook;
    void *prompt_user;
    bx_child_verbose_hook verbose_hook;
    void *verbose_user;
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
        .use_stdin_fd = false,
        .stdin_fd = -1,
        .use_stdout_fd = false,
        .stdout_fd = -1,
        .use_stderr_fd = false,
        .stderr_fd = -1,
        .reset_common_signals = false,
        .new_process_group = false,
        .prompt_hook = NULL,
        .prompt_user = NULL,
        .verbose_hook = NULL,
        .verbose_user = NULL,
    };
}

struct bx_child {
    pid_t pid;
    bool exec_failed;
    int exec_errno;
    int slot;
};

int bx_child_pick_slot(struct bx_child *children, int count, int max_procs);
void bx_child_signal_all(struct bx_child *children, int count, int signo);
int bx_child_exec_argv(char *const *argv);
int bx_child_exec_argv_exact_or_path(char *const *argv);
int bx_child_spawn_argv(const char *progname, char *const *argv,
                        const struct bx_child_runner_opts *opts,
                        int slot,
                        struct bx_child *children, int *running,
                        bool *exec_failed_now, int *exec_errno_now);
int bx_child_spawn_const_argv(const char *progname, const char *const *argv,
                              const struct bx_child_runner_opts *opts,
                              int slot,
                              struct bx_child *children, int *running,
                              bool *exec_failed_now, int *exec_errno_now);
int bx_child_reap(struct bx_child *children, int *running,
                  bool block, bool drain_all,
                  void (*cb)(pid_t pid, int status, bool exec_failed, int exec_errno, void *user),
                  void *user);

#endif
