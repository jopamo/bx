#ifndef BX_LIB_CHILD_RUNNER_H
#define BX_LIB_CHILD_RUNNER_H

#include <stdbool.h>
#include <signal.h>
#include <sys/types.h>

#include "backpressure_limit.h"
#include "cancel_state.h"
#include "workqueue_contract.h"

/*
 * Child action queues must follow BX_WORKQUEUE_CONTRACT_CHILD_ACTIONS:
 * an explicit BX_BACKPRESSURE_LIMIT_CHILD_PROCESSES slot bound plus
 * BX_BACKPRESSURE_LIMIT_OPEN_FDS fd budget, producer-owned argv/env/cwd/fd
 * actions before spawn, child_runner ownership after a successful slot claim,
 * producer cleanup after failed slot claim, and stop-spawning-before-reap
 * teardown.
 */

enum bx_child_prompt_result {
    BX_CHILD_PROMPT_ERROR = -1,
    BX_CHILD_PROMPT_SKIP = 0,
    BX_CHILD_PROMPT_RUN = 1,
};

enum bx_child_path_search_mode {
    BX_CHILD_PATH_SEARCH_STOP_ON_ERROR = 0,
    BX_CHILD_PATH_SEARCH_CONTINUE_ON_ERROR,
};

typedef int (*bx_child_prompt_hook)(const char *progname, char *const *argv, void *user);
typedef void (*bx_child_verbose_hook)(const char *progname, char *const *argv, void *user);
typedef int (*bx_child_parent_setup_hook)(pid_t pid, void *user);
typedef int (*bx_child_setup_hook)(void *user);
typedef void (*bx_child_exec_error_hook)(const char *executable, int errnum, void *user);
typedef int (*bx_child_fork_callback)(void *user);

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
    bool suppress_spawn_diagnostics;
    bool reset_tty_stop_signals;
    int parent_death_signal;
    bool new_process_group;
    bool wait_stdout_foreground;
    const char *executable;
    bool defer_exec_check;
    int setup_failure_status;
    int exec_failure_status;
    bx_child_setup_hook child_setup_hook;
    void *child_setup_user;
    bx_child_exec_error_hook child_exec_error_hook;
    void *child_exec_error_user;
    bx_child_prompt_hook prompt_hook;
    void *prompt_user;
    bx_child_verbose_hook verbose_hook;
    void *verbose_user;
    bx_child_parent_setup_hook parent_setup_hook;
    void *parent_setup_user;
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
        .suppress_spawn_diagnostics = false,
        .reset_tty_stop_signals = false,
        .parent_death_signal = 0,
        .new_process_group = false,
        .wait_stdout_foreground = false,
        .executable = NULL,
        .defer_exec_check = false,
        .setup_failure_status = 127,
        .exec_failure_status = 127,
        .child_setup_hook = NULL,
        .child_setup_user = NULL,
        .child_exec_error_hook = NULL,
        .child_exec_error_user = NULL,
        .prompt_hook = NULL,
        .prompt_user = NULL,
        .verbose_hook = NULL,
        .verbose_user = NULL,
        .parent_setup_hook = NULL,
        .parent_setup_user = NULL,
    };
}

struct bx_child {
    pid_t pid;
    bool exec_failed;
    int exec_errno;
    int slot;
};

int bx_child_pick_slot(struct bx_child *children, int count, int max_procs);
int bx_child_ensure_current_process_group(void);
int bx_child_signal_current_process_group(int signo, bool ignore_self);
void bx_child_signal_all(struct bx_child *children, int count, int signo);
int bx_child_finish_cancelled_run(struct bx_cancel_state *cancel,
                                  struct bx_child *children,
                                  int *running,
                                  int signo);
int bx_child_exec_argv(char *const *argv);
int bx_child_exec_file_argv_exact(const char *executable,
                                  char *const *argv);
int bx_child_exec_argv_exact_or_path(char *const *argv);
int bx_child_exec_file_argv(const char *executable, char *const *argv);
/*
 * Search the caller-supplied PATH without consulting process-global
 * environment state. Empty components are executed as explicit ./name
 * candidates so the caller-visible candidate identity remains stable.
 */
int bx_child_exec_file_argv_in_path(
    const char *executable,
    char *const *argv,
    const char *path,
    enum bx_child_path_search_mode mode);
int bx_child_fork_callback_wait(bx_child_fork_callback callback,
                                void *user,
                                int *status_out);
pid_t bx_child_fork_callback_start(bx_child_fork_callback callback,
                                   void *user);
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
int bx_child_reap_all_waitable(
    struct bx_child *children, int *running, int *reaped_count,
    void (*cb)(pid_t pid, int status, bool exec_failed, int exec_errno,
               void *user),
    void *user);

#endif
