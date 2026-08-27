/*
 * BusyBox init lifecycle and signal policy, adapted from BusyBox init/init.c
 * at bee252057c7ac69909b8aafeafb8e414e34c7685.
 * Copyright (C) 1995, 1996 Bruce Perens; Copyright (C) 1999-2004 Erik Andersen.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _GNU_SOURCE

#include "applets/system/init/init_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <syslog.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/vt.h>
#endif

#include "lib/child_runner.h"
#include "lib/fd_ops.h"
#include "lib/utmp_ops.h"
#include "lib/xreadwrite.h"

enum bx_init_message_target {
    BX_INIT_LOG = 0x01,
    BX_INIT_CONSOLE = 0x02,
};

struct bx_init_runtime {
    const char *progname;
    struct bx_init_table actions;
    sigset_t delayed_signals;
    struct timespec zero_timeout;
    struct bx_child *children;
    int running;
    int child_capacity;
    pid_t waited_pid;
    bool waited_pid_done;
};

struct bx_init_child_setup {
    struct bx_init_runtime *runtime;
    const struct bx_init_action *action;
    bool login_shell;
};

static volatile sig_atomic_t bx_init_continue_seen;
static struct bx_init_runtime *bx_init_signal_runtime;

static void bx_init_message(struct bx_init_runtime *runtime, int where,
                            const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void bx_init_message(struct bx_init_runtime *runtime, int where,
                            const char *format, ...) {
    char text[127];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    if ((where & BX_INIT_LOG) != 0) {
        openlog(runtime->progname, 0, LOG_DAEMON);
        syslog(LOG_INFO, "%s", text);
        closelog();
    }

    if ((where & BX_INIT_CONSOLE) != 0) {
        char console_text[130];
        int length = snprintf(console_text, sizeof(console_text), "\r%s\n", text);
        if (length > 0) {
            size_t count = (size_t)length;
            if (count >= sizeof(console_text))
                count = sizeof(console_text) - 1u;
            (void)bx_xwrite_all(STDERR_FILENO, console_text, count);
        }
    }
}

static void bx_init_fatal_pause(struct bx_init_runtime *runtime,
                                const char *operation) {
    int saved_errno = errno != 0 ? errno : EIO;
    bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                    "%s: %s", operation, strerror(saved_errno));
    for (;;) {
        struct timespec delay = {.tv_sec = 30 * 24 * 60 * 60, .tv_nsec = 0};
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            continue;
    }
}

static void bx_init_sanitize_stdio(void) {
    for (int target = STDIN_FILENO; target <= STDERR_FILENO; target++) {
        if (fcntl(target, F_GETFD) >= 0 || errno != EBADF)
            continue;
        int fd = bx_fd_open_cloexec("/dev/null", O_RDWR, 0);
        if (fd < 0)
            continue;
        if (bx_fd_dup2_exact(fd, target) < 0) {
            close(fd);
            continue;
        }
        (void)fcntl(target, F_SETFD, 0);
        if (fd != target)
            close(fd);
    }
}

static int bx_init_device_open(const char *path, int flags) {
    int open_flags = flags | O_NONBLOCK;
    int fd = -1;

    for (unsigned attempt = 0; attempt < 5u; attempt++) {
        fd = bx_fd_open_cloexec(path, open_flags, 0600);
        if (fd >= 0)
            break;
    }
    if (fd >= 0 && open_flags != flags)
        (void)fcntl(fd, F_SETFL, flags);
    return fd;
}

static void bx_init_console_init(struct bx_init_runtime *runtime) {
    const char *console = getenv("CONSOLE");
    if (console == NULL)
        console = getenv("console");

    if (console != NULL) {
        int fd = bx_init_device_open(
            console, O_RDWR | O_NONBLOCK | O_NOCTTY);
        if (fd >= 0) {
            (void)bx_fd_dup2_exact(fd, STDIN_FILENO);
            (void)bx_fd_dup2_exact(fd, STDOUT_FILENO);
            (void)bx_fd_dup2_exact(fd, STDERR_FILENO);
            (void)fcntl(STDIN_FILENO, F_SETFD, 0);
            (void)fcntl(STDOUT_FILENO, F_SETFD, 0);
            (void)fcntl(STDERR_FILENO, F_SETFD, 0);
            if (fd > STDERR_FILENO)
                close(fd);
        }
    } else {
        bx_init_sanitize_stdio();
    }

    const char *term = getenv("TERM");
#ifdef VT_OPENQRY
    int vt_number = 0;
    if (ioctl(STDIN_FILENO, VT_OPENQRY, &vt_number) != 0) {
        if (term == NULL || strcmp(term, "linux") == 0)
            (void)setenv("TERM", "vt102", 1);
    } else
#endif
    if (term == NULL) {
        (void)setenv("TERM", "linux", 1);
    }

    (void)runtime;
}

static void bx_init_set_sane_term(void) {
    struct termios terminal;
    if (tcgetattr(STDIN_FILENO, &terminal) != 0)
        return;

    terminal.c_cc[VINTR] = 3;
    terminal.c_cc[VQUIT] = 28;
    terminal.c_cc[VERASE] = 127;
    terminal.c_cc[VKILL] = 21;
    terminal.c_cc[VEOF] = 4;
    terminal.c_cc[VSTART] = 17;
    terminal.c_cc[VSTOP] = 19;
    terminal.c_cc[VSUSP] = 26;
#ifdef __linux__
    terminal.c_line = 0;
#endif

#ifndef CBAUD
#define CBAUD 0
#endif
#ifndef CBAUDEX
#define CBAUDEX 0
#endif
#ifndef CRTSCTS
#define CRTSCTS 0
#endif
    terminal.c_cflag &=
        CBAUD | CBAUDEX | CSIZE | CSTOPB | PARENB | PARODD | CRTSCTS;
    terminal.c_cflag |= CREAD | HUPCL | CLOCAL;
    terminal.c_iflag = ICRNL | IXON | IXOFF;
    terminal.c_oflag = OPOST | ONLCR;
    terminal.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
#ifdef ECHOCTL
    terminal.c_lflag |= ECHOCTL;
#endif
#ifdef ECHOKE
    terminal.c_lflag |= ECHOKE;
#endif
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &terminal);
}

static bool bx_init_open_stdio_to_tty(struct bx_init_runtime *runtime,
                                      const char *terminal) {
    if (terminal[0] != '\0') {
        close(STDIN_FILENO);
        int fd = bx_init_device_open(terminal, O_RDWR);
        if (fd != STDIN_FILENO) {
            int saved_errno = errno;
            if (fd >= 0)
                close(fd);
            bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                            "can't open %s: %s",
                            terminal, strerror(saved_errno));
            errno = saved_errno;
            return false;
        }

        if (fcntl(STDIN_FILENO, F_SETFD, 0) < 0 ||
            bx_fd_dup2_exact(STDIN_FILENO, STDOUT_FILENO) < 0 ||
            bx_fd_dup2_exact(fd, STDERR_FILENO) < 0) {
            int saved_errno = errno;
            bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                            "can't open %s: %s",
                            terminal, strerror(saved_errno));
            errno = saved_errno;
            return false;
        }
    }

    bx_init_set_sane_term();
    return true;
}

static void bx_init_reset_child_signals(void) {
    (void)signal(SIGTSTP, SIG_DFL);
    (void)signal(SIGCONT, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
}

static int bx_init_child_setup(void *user) {
    struct bx_init_child_setup *setup = user;
    const struct bx_init_action *action = setup->action;

    bx_init_reset_child_signals();
    (void)setsid();

    if (!bx_init_open_stdio_to_tty(setup->runtime, action->terminal))
        return -1;

    if ((action->action_type & BX_INIT_ASKFIRST) != 0) {
        static const char prompt[] =
            "\nPlease press Enter to activate this console. ";
        char byte;
        (void)bx_xwrite_all(STDOUT_FILENO, prompt, sizeof(prompt) - 1u);
        while (bx_xread(STDIN_FILENO, &byte, 1u) == 1 && byte != '\n')
            continue;
    }

    bx_init_message(setup->runtime, BX_INIT_LOG,
                    "starting pid %u, tty '%s': '%s'",
                    (unsigned)getpid(), action->terminal, action->command);
    if (setup->login_shell)
        (void)ioctl(STDIN_FILENO, TIOCSCTTY, 0);
    return 0;
}

static void bx_init_child_exec_error(const char *executable,
                                     int errnum,
                                     void *user) {
    struct bx_init_runtime *runtime = user;
    bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                    "can't run '%s': %s", executable, strerror(errnum));
}

static bool bx_init_reserve_child(struct bx_init_runtime *runtime) {
    if (runtime->running < runtime->child_capacity)
        return true;

    if (runtime->child_capacity > INT_MAX / 2) {
        errno = ENOMEM;
        return false;
    }
    int capacity = runtime->child_capacity == 0
        ? 16
        : runtime->child_capacity * 2;
    if ((size_t)capacity > SIZE_MAX / sizeof(*runtime->children)) {
        errno = ENOMEM;
        return false;
    }
    struct bx_child *children = realloc(
        runtime->children, (size_t)capacity * sizeof(*children));
    if (children == NULL)
        return false;
    runtime->children = children;
    runtime->child_capacity = capacity;
    return true;
}

static pid_t bx_init_spawn(struct bx_init_runtime *runtime,
                           struct bx_init_action *action) {
    struct bx_init_command command;
    if (!bx_init_command_build(action->command, &command)) {
        int saved_errno = errno != 0 ? errno : EIO;
        if (saved_errno == ENOMEM)
            bx_init_fatal_pause(runtime, "can't allocate child command");
        bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                        "can't run '%s': %s",
                        action->command, strerror(saved_errno));
        return -1;
    }
    if (!bx_init_reserve_child(runtime)) {
        bx_init_command_destroy(&command);
        if (errno == ENOMEM)
            bx_init_fatal_pause(runtime, "can't allocate child state");
        bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                        "can't fork");
        return -1;
    }

    struct bx_init_child_setup child_setup = {
        .runtime = runtime,
        .action = action,
        .login_shell = command.login_shell,
    };
    struct bx_child_runner_opts options = bx_child_runner_opts_default();
    options.reset_common_signals = true;
    options.suppress_spawn_diagnostics = true;
    options.executable = command.executable;
    options.defer_exec_check =
        (action->action_type & BX_INIT_ASKFIRST) != 0;
    options.setup_failure_status = 1;
    options.exec_failure_status = 255;
    options.child_setup_hook = bx_init_child_setup;
    options.child_setup_user = &child_setup;
    options.child_exec_error_hook = bx_init_child_exec_error;
    options.child_exec_error_user = runtime;

    bool exec_failed = false;
    int exec_errno = 0;
    int old_running = runtime->running;
    int rc = bx_child_spawn_argv(
        runtime->progname, command.argv, &options, 0,
        runtime->children, &runtime->running,
        &exec_failed, &exec_errno);
    bx_init_command_destroy(&command);
    if (rc != 0) {
        bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                        "can't fork");
        return -1;
    }
    if (runtime->running == old_running)
        return -1;

    (void)exec_failed;
    (void)exec_errno;
    return runtime->children[runtime->running - 1].pid;
}

static struct bx_init_action *bx_init_mark_terminated(
    struct bx_init_runtime *runtime, pid_t pid) {
    if (pid <= 0)
        return NULL;
    bx_utmp_mark_dead(pid);
    for (struct bx_init_action *action = runtime->actions.head;
         action != NULL;
         action = action->next) {
        if (action->pid == pid) {
            action->pid = 0;
            return action;
        }
    }
    return NULL;
}

static void bx_init_reaped(pid_t pid, int status,
                           bool exec_failed, int exec_errno, void *user) {
    struct bx_init_runtime *runtime = user;
    struct bx_init_action *action = bx_init_mark_terminated(runtime, pid);

    if (runtime->waited_pid == pid)
        runtime->waited_pid_done = true;

    if (action != NULL) {
        const char *description = "killed, signal";
        int result = WTERMSIG(status);
        if (WIFEXITED(status)) {
            description = "exited, exitcode";
            result = WEXITSTATUS(status);
        }
        bx_init_message(runtime, BX_INIT_LOG,
                        "process '%s' (pid %u) %s:%d. Scheduling for restart.",
                        action->command, (unsigned)pid, description, result);
    }

    (void)exec_failed;
    (void)exec_errno;
}

static void bx_init_wait_for(struct bx_init_runtime *runtime, pid_t pid) {
    if (pid <= 0)
        return;

    runtime->waited_pid = pid;
    runtime->waited_pid_done = false;
    while (!runtime->waited_pid_done) {
        if (bx_child_reap(runtime->children, &runtime->running,
                          true, false, bx_init_reaped, runtime) != 0)
            break;
        if (!runtime->waited_pid_done && kill(pid, 0) != 0)
            runtime->waited_pid_done = true;
    }
    runtime->waited_pid = 0;
}

static void bx_init_run_actions(struct bx_init_runtime *runtime,
                                uint8_t action_types) {
    for (struct bx_init_action *action = runtime->actions.head;
         action != NULL;
         action = action->next) {
        if ((action->action_type & action_types) == 0)
            continue;

        if ((action->action_type &
             (BX_INIT_SYSINIT | BX_INIT_WAIT | BX_INIT_ONCE |
              BX_INIT_CTRLALTDEL | BX_INIT_SHUTDOWN)) != 0) {
            pid_t pid = bx_init_spawn(runtime, action);
            if ((action->action_type &
                 (BX_INIT_SYSINIT | BX_INIT_WAIT |
                  BX_INIT_CTRLALTDEL | BX_INIT_SHUTDOWN)) != 0)
                bx_init_wait_for(runtime, pid);
        }

        if ((action->action_type &
             (BX_INIT_RESPAWN | BX_INIT_ASKFIRST)) != 0 &&
            action->pid == 0)
            action->pid = bx_init_spawn(runtime, action);
    }
}

static void bx_init_config_bad_entry(void *user, unsigned line_number) {
    struct bx_init_runtime *runtime = user;
    bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                    "Bad inittab entry at line %u", line_number);
}

#if BX_INIT_FEATURE_KILL_REMOVED && BX_INIT_FEATURE_KILL_DELAY
static int bx_init_kill_removed_after_delay(void *user) {
    struct bx_init_runtime *runtime = user;
    sleep(BX_INIT_FEATURE_KILL_DELAY);
    for (struct bx_init_action *action = runtime->actions.head;
         action != NULL;
         action = action->next) {
        if (action->action_type == 0 && action->pid != 0)
            (void)kill(action->pid, SIGKILL);
    }
    return 0;
}
#endif

static void bx_init_reload(struct bx_init_runtime *runtime) {
    bx_init_message(runtime, BX_INIT_LOG, "reloading /etc/inittab");
    if (!bx_init_table_reload_in_place(
            &runtime->actions, BX_INIT_DEFAULT_INITTAB,
            bx_init_config_bad_entry, runtime))
        bx_init_fatal_pause(runtime, "can't allocate inittab entry");

#if BX_INIT_FEATURE_KILL_REMOVED
    for (struct bx_init_action *action = runtime->actions.head;
         action != NULL;
         action = action->next) {
        if (action->action_type == 0 && action->pid != 0)
            (void)kill(action->pid, SIGTERM);
    }
#if BX_INIT_FEATURE_KILL_DELAY
    (void)bx_child_fork_callback_start(
        bx_init_kill_removed_after_delay, runtime);
#endif
#endif

    struct bx_init_action **link = &runtime->actions.head;
    while (*link != NULL) {
        struct bx_init_action *action = *link;
        if ((action->action_type & (uint8_t)~BX_INIT_SYSINIT) == 0 &&
            action->pid == 0) {
            *link = action->next;
            free(action);
        } else {
            link = &action->next;
        }
    }
}

static void bx_init_reset_parent_signals(struct bx_init_runtime *runtime) {
    (void)signal(SIGTSTP, SIG_DFL);
    (void)signal(SIGCONT, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    (void)sigprocmask(SIG_SETMASK, &empty, NULL);
    (void)runtime;
}

static void bx_init_run_shutdown_and_kill(struct bx_init_runtime *runtime) {
    bx_init_run_actions(runtime, BX_INIT_SHUTDOWN);
    bx_init_message(runtime, BX_INIT_CONSOLE | BX_INIT_LOG,
                    "The system is going down NOW!");

    (void)kill(-1, SIGTERM);
    bx_init_message(runtime, BX_INIT_CONSOLE,
                    "Sent SIGTERM to all processes");
    sync();
    sleep(1);

    (void)kill(-1, SIGKILL);
    bx_init_message(runtime, BX_INIT_CONSOLE,
                    "Sent SIGKILL to all processes");
    sync();
}

static void bx_init_low_level_reboot(unsigned command)
    __attribute__((noreturn));

static int bx_init_reboot_child(void *user) {
    unsigned command = *(const unsigned *)user;
    (void)reboot((int)command);
    return 0;
}

static void bx_init_low_level_reboot(unsigned command) {
    sleep(1);
    (void)bx_child_fork_callback_wait(
        bx_init_reboot_child, &command, NULL);
    sleep(1);
    _exit(0);
}

static void bx_init_halt_reboot_poweroff(struct bx_init_runtime *runtime,
                                         int signal_number)
    __attribute__((noreturn));

static void bx_init_halt_reboot_poweroff(struct bx_init_runtime *runtime,
                                         int signal_number) {
    bx_init_reset_parent_signals(runtime);
    bx_init_run_shutdown_and_kill(runtime);

    const char *name = "halt";
    unsigned command = RB_HALT_SYSTEM;
    if (signal_number == SIGTERM) {
        name = "reboot";
        command = RB_AUTOBOOT;
    } else if (signal_number == SIGUSR2) {
        name = "poweroff";
        command = RB_POWER_OFF;
    }
    bx_init_message(runtime, BX_INIT_CONSOLE,
                    "Requesting system %s", name);
    bx_init_low_level_reboot(command);
}

static void bx_init_exec_restart(struct bx_init_runtime *runtime) {
    struct bx_init_action *restart = NULL;
    for (struct bx_init_action *action = runtime->actions.head;
         action != NULL;
         action = action->next) {
        if ((action->action_type & BX_INIT_RESTART) != 0) {
            restart = action;
            break;
        }
    }
    if (restart == NULL)
        return;

    bx_init_reset_parent_signals(runtime);
    bx_init_run_shutdown_and_kill(runtime);
#ifdef RB_ENABLE_CAD
    (void)reboot(RB_ENABLE_CAD);
#endif

    if (bx_init_open_stdio_to_tty(runtime, restart->terminal)) {
        struct bx_init_command command;
        if (bx_init_command_build(restart->command, &command)) {
            if (command.login_shell)
                (void)ioctl(STDIN_FILENO, TIOCSCTTY, 0);
            int errnum = bx_child_exec_file_argv(
                command.executable, command.argv);
            bx_init_message(runtime, BX_INIT_LOG | BX_INIT_CONSOLE,
                            "can't run '%s': %s",
                            command.executable, strerror(errnum));
            bx_init_command_destroy(&command);
        }
    }
    bx_init_low_level_reboot(RB_HALT_SYSTEM);
}

static void bx_init_check_delayed_signals(struct bx_init_runtime *runtime,
                                          const struct timespec *timeout) {
    int signal_number = sigtimedwait(
        &runtime->delayed_signals, NULL, timeout);
    if (signal_number <= 0)
        return;

    if (signal_number == SIGHUP)
        bx_init_reload(runtime);
    else if (signal_number == SIGINT)
        bx_init_run_actions(runtime, BX_INIT_CTRLALTDEL);
    else if (signal_number == SIGQUIT)
        bx_init_exec_restart(runtime);
    else if (signal_number == SIGUSR1 ||
             signal_number == SIGTERM ||
             signal_number == SIGUSR2
#ifdef SIGPWR
             || signal_number == SIGPWR
#endif
    ) {
        bx_init_halt_reboot_poweroff(runtime, signal_number);
    }
}

static void bx_init_continue_handler(int signal_number) {
    bx_init_continue_seen = signal_number;
}

static void bx_init_reaped_while_stopped(
    pid_t pid, int status, bool exec_failed, int exec_errno, void *user) {
    struct bx_init_runtime *runtime = user;
    (void)bx_init_mark_terminated(runtime, pid);
    if (runtime->waited_pid == pid)
        runtime->waited_pid_done = true;
    (void)status;
    (void)exec_failed;
    (void)exec_errno;
}

static void bx_init_stop_handler(int signal_number) {
    (void)signal_number;
    int saved_errno = errno;
    bx_init_continue_seen = 0;

    struct sigaction continue_action;
    memset(&continue_action, 0, sizeof(continue_action));
    sigfillset(&continue_action.sa_mask);
    continue_action.sa_handler = bx_init_continue_handler;
    (void)sigaction(SIGCONT, &continue_action, NULL);

    while (bx_init_continue_seen != SIGCONT) {
        int reaped = 0;
        if (bx_init_signal_runtime != NULL) {
            (void)bx_child_reap_all_waitable(
                bx_init_signal_runtime->children,
                &bx_init_signal_runtime->running,
                &reaped,
                bx_init_reaped_while_stopped,
                bx_init_signal_runtime);
        }
        if (reaped == 0)
            sleep(1);
    }

    (void)signal(SIGCONT, SIG_DFL);
    errno = saved_errno;
}

static void bx_init_install_stop_handler(struct bx_init_runtime *runtime) {
    struct sigaction stop_action;
    memset(&stop_action, 0, sizeof(stop_action));
    sigfillset(&stop_action.sa_mask);
    sigdelset(&stop_action.sa_mask, SIGCONT);
    stop_action.sa_handler = bx_init_stop_handler;
    stop_action.sa_flags = SA_RESTART;
    if (sigaction(SIGTSTP, &stop_action, NULL) != 0)
        bx_init_fatal_pause(runtime, "can't install signal handler");
}

static void bx_init_mask_delayed_signals(struct bx_init_runtime *runtime) {
    sigemptyset(&runtime->delayed_signals);
    sigaddset(&runtime->delayed_signals, SIGINT);
    sigaddset(&runtime->delayed_signals, SIGQUIT);
#ifdef SIGPWR
    sigaddset(&runtime->delayed_signals, SIGPWR);
#endif
    sigaddset(&runtime->delayed_signals, SIGUSR1);
    sigaddset(&runtime->delayed_signals, SIGTERM);
    sigaddset(&runtime->delayed_signals, SIGUSR2);
    sigaddset(&runtime->delayed_signals, SIGHUP);
    sigaddset(&runtime->delayed_signals, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &runtime->delayed_signals, NULL) != 0)
        bx_init_fatal_pause(runtime, "can't block signals");
}

static void bx_init_modify_command_line(char **argv) {
    if (argv == NULL || argv[0] == NULL)
        return;
    size_t argv0_len = strlen(argv[0]);
    if (argv0_len > 0u)
        (void)strncpy(argv[0], "init", argv0_len);
    for (size_t i = 1u; argv[i] != NULL; i++)
        memset(argv[i], 0, strlen(argv[i]));
}

int bx_init_run(int argc, char **argv, const char *progname) {
    struct bx_init_runtime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.progname = progname;
    bx_init_table_init(&runtime.actions);
    bx_init_mask_delayed_signals(&runtime);

    if (argc > 1 && strcmp(argv[1], "-q") == 0)
        return kill(1, SIGHUP);

    if (getpid() != 1 && strcmp(progname, "linuxrc") != 0) {
        fprintf(stderr, "%s: must be run as PID 1\n", progname);
        return 1;
    }

#ifdef RB_DISABLE_CAD
    (void)reboot(RB_DISABLE_CAD);
#endif

    bx_init_console_init(&runtime);
    bx_init_set_sane_term();
    if (chdir("/") != 0)
        bx_init_fatal_pause(&runtime, "can't chdir to /");
    (void)setsid();

    if (setenv("PATH", "/sbin:/usr/sbin:/bin:/usr/bin", 1) != 0 ||
        setenv("SHELL", BX_INIT_DEFAULT_SHELL, 1) != 0 ||
        setenv("USER", "root", 1) != 0)
        bx_init_fatal_pause(&runtime, "can't set environment");
    if (argc > 1 && setenv("RUNLEVEL", argv[1], 1) != 0)
        bx_init_fatal_pause(&runtime, "can't set RUNLEVEL");

    bool single_user = argc > 1 &&
        (strcmp(argv[1], "single") == 0 ||
         strcmp(argv[1], "-s") == 0 ||
         strcmp(argv[1], "1") == 0);
    if (single_user) {
        struct bx_init_table single;
        bx_init_table_init(&single);
        struct bx_init_action *action = calloc(
            1u, sizeof(*action) + sizeof("-" BX_INIT_DEFAULT_SHELL));
        if (action == NULL)
            bx_init_fatal_pause(&runtime, "can't allocate init action");
        action->action_type = BX_INIT_RESPAWN;
        memcpy(action->command, "-" BX_INIT_DEFAULT_SHELL,
               sizeof("-" BX_INIT_DEFAULT_SHELL));
        single.head = action;
        runtime.actions = single;
    } else if (!bx_init_table_load(
                   &runtime.actions, BX_INIT_DEFAULT_INITTAB,
                   bx_init_config_bad_entry, &runtime)) {
        bx_init_fatal_pause(&runtime, "can't read /etc/inittab");
    }

    bx_init_modify_command_line(argv);
    bx_init_signal_runtime = &runtime;
    bx_init_install_stop_handler(&runtime);

    bx_init_run_actions(&runtime, BX_INIT_SYSINIT);
    bx_init_check_delayed_signals(&runtime, &runtime.zero_timeout);
    bx_init_run_actions(&runtime, BX_INIT_WAIT);
    bx_init_check_delayed_signals(&runtime, &runtime.zero_timeout);
    bx_init_run_actions(&runtime, BX_INIT_ONCE);

    for (;;) {
        bx_init_run_actions(
            &runtime, BX_INIT_RESPAWN | BX_INIT_ASKFIRST);
        bx_init_check_delayed_signals(&runtime, NULL);
        (void)bx_child_reap_all_waitable(
            runtime.children, &runtime.running, NULL,
            bx_init_reaped, &runtime);
        sleep(1);
    }
}
