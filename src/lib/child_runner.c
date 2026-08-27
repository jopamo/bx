#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <unistd.h>
#include "child_runner.h"
#include "fd_ops.h"

extern char **environ;

static struct bx_child *bx_child_find(struct bx_child *children, int count, pid_t pid) {
    for (int i = 0; i < count; i++) {
        if (children[i].pid == pid)
            return &children[i];
    }
    return NULL;
}

int bx_child_pick_slot(struct bx_child *children, int count, int max_procs) {
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

int bx_child_ensure_current_process_group(void) {
    if (setpgid(0, 0) == 0)
        return 0;

    int saved_errno = errno;
    if (getpgrp() == getpid())
        return 0;

    errno = saved_errno;
    return -1;
}

int bx_child_signal_current_process_group(int signo, bool ignore_self) {
    if (signo < 0) {
        errno = EINVAL;
        return -1;
    }

    if (ignore_self && signo > 0) {
        struct sigaction ignored_action;
        memset(&ignored_action, 0, sizeof(ignored_action));
        ignored_action.sa_handler = SIG_IGN;
        sigemptyset(&ignored_action.sa_mask);
        if (sigaction(signo, &ignored_action, NULL) != 0 &&
            signo != SIGKILL && signo != SIGSTOP)
            return -1;
    }

    return kill(0, signo);
}

void bx_child_signal_all(struct bx_child *children, int count, int signo) {
    if (!children || count <= 0 || signo <= 0)
        return;

    for (int i = 0; i < count; i++) {
        if (children[i].pid > 0 && kill(children[i].pid, signo) < 0 && errno != ESRCH)
            continue;
    }
}

int bx_child_finish_cancelled_run(struct bx_cancel_state *cancel,
                                  struct bx_child *children,
                                  int *running,
                                  int signo) {
    if (signo == 0)
        return 0;
    if (!children || !running)
        return 1;

    if (cancel) {
        (void)bx_cancel_state_mark_requested(cancel);
        (void)bx_cancel_state_mark_observed(cancel);
        (void)bx_cancel_state_mark_draining(cancel);
    }

    bx_child_signal_all(children, *running, signo);
    if (cancel && *running > 0)
        (void)bx_cancel_state_mark_killed(cancel);

    while (*running > 0) {
        if (bx_child_reap(children, running, true, true, NULL, NULL) != 0)
            return 1;
    }

    if (cancel)
        (void)bx_cancel_state_mark_joined(cancel);
    return 128 + signo;
}

int bx_child_exec_argv(char *const *argv) {
    return bx_child_exec_argv_exact_or_path(argv);
}

static int bx_child_exec_exact(const char *executable, char *const *argv) {
    if (!executable || !argv || !argv[0])
        return EINVAL;

    execve(executable, argv, environ);
    return errno != 0 ? errno : EIO;
}

int bx_child_exec_argv_exact_or_path(char *const *argv) {
    if (!argv || !argv[0])
        return EINVAL;
    return bx_child_exec_file_argv(argv[0], argv);
}

int bx_child_exec_file_argv(const char *executable, char *const *argv) {
    if (!executable || !argv || !argv[0])
        return EINVAL;
    if (strchr(executable, '/'))
        return bx_child_exec_exact(executable, argv);

    const char *command = executable;
    size_t command_len = strlen(command);
    if (command_len == 0)
        return ENOENT;

    const char *path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";

    bool saw_eacces = false;
    const char *segment = path;
    for (;;) {
        size_t dir_len = strcspn(segment, ":");
        bool has_colon = segment[dir_len] == ':';
        char *candidate_path = NULL;

        if (dir_len == 0) {
            candidate_path = malloc(command_len + 1);
            if (!candidate_path)
                return ENOMEM;
            memcpy(candidate_path, command, command_len + 1);
        } else {
            if (dir_len > SIZE_MAX - 2 || command_len > SIZE_MAX - dir_len - 2)
                return ENAMETOOLONG;
            size_t candidate_len = dir_len + 1 + command_len;
            candidate_path = malloc(candidate_len + 1);
            if (!candidate_path)
                return ENOMEM;
            memcpy(candidate_path, segment, dir_len);
            candidate_path[dir_len] = '/';
            memcpy(candidate_path + dir_len + 1, command, command_len + 1);
        }

        execve(candidate_path, argv, environ);
        int err = errno != 0 ? errno : EIO;
        free(candidate_path);

        if (err == EACCES) {
            saw_eacces = true;
        } else if (err != ENOENT && err != ENOTDIR) {
            return err;
        }

        if (!has_colon)
            break;
        segment += dir_len + 1;
    }

    return saw_eacces ? EACCES : ENOENT;
}

pid_t bx_child_fork_callback_start(bx_child_fork_callback callback,
                                   void *user) {
    if (!callback) {
        errno = EINVAL;
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int status = callback(user);
        _exit(status >= 0 && status <= 255 ? status : 255);
    }
    return pid;
}

int bx_child_fork_callback_wait(bx_child_fork_callback callback,
                                void *user,
                                int *status_out) {
    pid_t pid = bx_child_fork_callback_start(callback, user);
    if (pid < 0)
        return -1;

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    if (status_out)
        *status_out = status;
    return 0;
}

static int bx_child_dup_stdio_fd(int source_fd, int target_fd) {
    if (source_fd < 0)
        return EBADF;
    if (source_fd == target_fd)
        return 0;
    if (bx_fd_dup2_exact(source_fd, target_fd) < 0)
        return errno != 0 ? errno : EIO;
    return 0;
}

static void bx_child_close_redirect_fd(int fd, int target_fd,
                                       int already_closed_1, int already_closed_2) {
    if (fd < 0 || fd == target_fd || fd == already_closed_1 || fd == already_closed_2)
        return;
    close(fd);
}

static void bx_child_reset_common_signal_handlers(void) {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

static void bx_child_reset_tty_stop_signal_handlers(void) {
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

static int bx_child_failure_status(int configured_status) {
    return configured_status > 0 && configured_status <= 255
        ? configured_status
        : 127;
}

static void bx_child_report_exec_error(
    int fd,
    int errnum,
    const struct bx_child_runner_opts *opts) {
    if (!opts || !opts->defer_exec_check)
        (void)!write(fd, &errnum, sizeof(errnum));
}

static int bx_child_wait_stdout_foreground(void) {
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 10000000L,
    };

    for (;;) {
        pid_t foreground = tcgetpgrp(STDOUT_FILENO);
        if (foreground == getpid())
            return 0;
        if (foreground < 0)
            return 0;
        while (nanosleep(&delay, &delay) != 0) {
            if (errno != EINTR)
                return errno != 0 ? errno : EIO;
        }
        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
    }
}

int bx_child_spawn_argv(const char *progname, char *const *argv,
                        const struct bx_child_runner_opts *opts,
                        int slot,
                        struct bx_child *children, int *running,
                        bool *exec_failed_now, int *exec_errno_now) {
    if (exec_failed_now)
        *exec_failed_now = false;
    if (exec_errno_now)
        *exec_errno_now = 0;

    if (opts && opts->prompt_hook) {
        int prompt_rc = opts->prompt_hook(progname, argv, opts->prompt_user);
        if (prompt_rc == BX_CHILD_PROMPT_ERROR)
            return 1;
        if (prompt_rc == BX_CHILD_PROMPT_SKIP)
            return 0;
    }

    int errpipe[2];
    if (bx_fd_pipe_cloexec(errpipe) != 0) {
        if (!opts || !opts->suppress_spawn_diagnostics)
            fprintf(stderr, "%s: pipe failed: %s\n", progname, strerror(errno));
        return 1;
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        if (!opts || !opts->suppress_spawn_diagnostics)
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
        if (opts && opts->reset_common_signals)
            bx_child_reset_common_signal_handlers();
        if (opts && opts->reset_tty_stop_signals)
            bx_child_reset_tty_stop_signal_handlers();
#ifdef __linux__
        if (opts && opts->parent_death_signal > 0) {
            if (prctl(PR_SET_PDEATHSIG, opts->parent_death_signal) != 0) {
                errnum = errno != 0 ? errno : EIO;
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
            if (getppid() != parent_pid) {
                errnum = ECANCELED;
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
        }
#else
        (void)parent_pid;
#endif
        if (opts && opts->new_process_group && setpgid(0, 0) != 0) {
            errnum = errno;
            bx_child_report_exec_error(errpipe[1], errnum, opts);
            _exit(127);
        }
        if (opts && opts->child_setup_hook &&
            opts->child_setup_hook(opts->child_setup_user) != 0) {
            errnum = errno != 0 ? errno : EIO;
            bx_child_report_exec_error(errpipe[1], errnum, opts);
            _exit(bx_child_failure_status(opts->setup_failure_status));
        }
        if (opts && opts->reopen_stdin_tty) {
            int tty_fd = bx_fd_open_cloexec("/dev/tty", O_RDONLY, 0);
            if (tty_fd < 0) {
                errnum = errno;
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
            if (bx_fd_dup2_exact(tty_fd, STDIN_FILENO) < 0) {
                errnum = errno;
                close(tty_fd);
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
            if (tty_fd != STDIN_FILENO)
                close(tty_fd);
        }
        if (opts && opts->use_stdin_fd) {
            errnum = bx_child_dup_stdio_fd(opts->stdin_fd, STDIN_FILENO);
            if (errnum != 0) {
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
        }
        if (opts && opts->use_stdout_fd) {
            errnum = bx_child_dup_stdio_fd(opts->stdout_fd, STDOUT_FILENO);
            if (errnum != 0) {
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
        }
        if (opts && opts->use_stderr_fd) {
            errnum = bx_child_dup_stdio_fd(opts->stderr_fd, STDERR_FILENO);
            if (errnum != 0) {
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
        }
        if (opts && opts->use_stdin_fd)
            bx_child_close_redirect_fd(opts->stdin_fd, STDIN_FILENO, -1, -1);
        if (opts && opts->use_stdout_fd)
            bx_child_close_redirect_fd(opts->stdout_fd, STDOUT_FILENO,
                                       opts && opts->use_stdin_fd ? opts->stdin_fd : -1, -1);
        if (opts && opts->use_stderr_fd)
            bx_child_close_redirect_fd(opts->stderr_fd, STDERR_FILENO,
                                       opts && opts->use_stdin_fd ? opts->stdin_fd : -1,
                                       opts && opts->use_stdout_fd ? opts->stdout_fd : -1);
        if (opts && opts->cwd && chdir(opts->cwd) != 0) {
            errnum = errno;
            bx_child_report_exec_error(errpipe[1], errnum, opts);
            _exit(127);
        }
        if (opts && opts->wait_stdout_foreground) {
            errnum = bx_child_wait_stdout_foreground();
            if (errnum != 0) {
                bx_child_report_exec_error(errpipe[1], errnum, opts);
                _exit(127);
            }
        }
        errnum = opts && opts->executable
            ? bx_child_exec_file_argv(opts->executable, argv)
            : bx_child_exec_argv(argv);
        if (opts && opts->child_exec_error_hook) {
            opts->child_exec_error_hook(
                opts->executable ? opts->executable : argv[0],
                errnum,
                opts->child_exec_error_user);
        }
        bx_child_report_exec_error(errpipe[1], errnum, opts);
        _exit(bx_child_failure_status(
            opts ? opts->exec_failure_status : 127));
    }

    close(errpipe[1]);
    if (opts && opts->parent_setup_hook &&
        opts->parent_setup_hook(pid, opts->parent_setup_user) != 0) {
        int saved_errno = errno != 0 ? errno : EIO;
        kill(pid, SIGTERM);
        close(errpipe[0]);
        (void)waitpid(pid, NULL, 0);
        errno = saved_errno;
        return 1;
    }

    if (opts && opts->verbose_hook) {
        opts->verbose_hook(progname, argv, opts->verbose_user);
    } else if (opts && opts->verbose) {
        for (int i = 0; argv[i]; i++)
            fprintf(stderr, "%s%s", i == 0 ? "" : " ", argv[i]);
        fputc('\n', stderr);
    }

    children[*running].pid = pid;
    children[*running].exec_failed = false;
    children[*running].exec_errno = 0;
    children[*running].slot = slot;
    (*running)++;

    int exec_errno = 0;
    ssize_t nread = 0;
    if (!opts || !opts->defer_exec_check)
        nread = read(errpipe[0], &exec_errno, sizeof(exec_errno));
    close(errpipe[0]);

    if (nread == (ssize_t)sizeof(exec_errno)) {
        children[*running - 1].exec_failed = true;
        children[*running - 1].exec_errno = exec_errno;
        if (exec_failed_now)
            *exec_failed_now = true;
        if (exec_errno_now)
            *exec_errno_now = exec_errno;
    }

    return 0;
}

int bx_child_spawn_const_argv(const char *progname, const char *const *argv,
                              const struct bx_child_runner_opts *opts,
                              int slot,
                              struct bx_child *children, int *running,
                              bool *exec_failed_now, int *exec_errno_now) {
    size_t argc = 0u;
    char **mutable_argv = NULL;

    if (!argv || !argv[0]) {
        if (exec_failed_now)
            *exec_failed_now = true;
        if (exec_errno_now)
            *exec_errno_now = EINVAL;
        return 1;
    }

    while (argv[argc])
        argc++;
    mutable_argv = calloc(argc + 1u, sizeof(*mutable_argv));
    if (!mutable_argv) {
        fprintf(stderr, "%s: cannot allocate child argv: %s\n", progname, strerror(errno));
        return 1;
    }
    for (size_t i = 0; i < argc; i++) {
        mutable_argv[i] = strdup(argv[i]);
        if (!mutable_argv[i]) {
            int saved_errno = errno;
            for (size_t j = 0; j < i; j++)
                free(mutable_argv[j]);
            free(mutable_argv);
            fprintf(stderr, "%s: cannot allocate child argv: %s\n", progname, strerror(saved_errno));
            return 1;
        }
    }

    int rc = bx_child_spawn_argv(progname, mutable_argv, opts, slot, children,
                                 running, exec_failed_now, exec_errno_now);
    for (size_t i = 0; i < argc; i++)
        free(mutable_argv[i]);
    free(mutable_argv);
    return rc;
}

int bx_child_reap(struct bx_child *children, int *running,
                  bool block, bool drain_all,
                  void (*cb)(pid_t pid, int status, bool exec_failed, int exec_errno, void *user),
                  void *user) {
    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, block ? 0 : WNOHANG);
        if (pid == 0)
            return 0;
        if (pid < 0) {
            if (errno == EINTR)
                return 0;
            if (!block && errno == ECHILD)
                return 0;
            return (errno == ECHILD) ? 0 : 1;
        }

        struct bx_child *child = bx_child_find(children, *running, pid);
        bool exec_failed = child && child->exec_failed;
        int exec_errno = child ? child->exec_errno : 0;
        if (child) {
            *child = children[*running - 1];
            (*running)--;
        }

        if (cb)
            cb(pid, status, exec_failed, exec_errno, user);
        if (!drain_all)
            return 0;
        if (*running == 0)
            return 0;
        block = false;
    }
}

int bx_child_reap_all_waitable(
    struct bx_child *children, int *running, int *reaped_count,
    void (*cb)(pid_t pid, int status, bool exec_failed, int exec_errno,
               void *user),
    void *user) {
    int count = 0;

    if (!running || (*running > 0 && !children)) {
        errno = EINVAL;
        return 1;
    }

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0 || (pid < 0 && errno == ECHILD)) {
            if (reaped_count)
                *reaped_count = count;
            return 0;
        }
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            if (reaped_count)
                *reaped_count = count;
            return 1;
        }

        struct bx_child *child = bx_child_find(children, *running, pid);
        bool exec_failed = child && child->exec_failed;
        int exec_errno = child ? child->exec_errno : 0;
        if (child) {
            *child = children[*running - 1];
            (*running)--;
        }
        count++;
        if (cb)
            cb(pid, status, exec_failed, exec_errno, user);
    }
}
