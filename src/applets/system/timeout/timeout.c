#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/child_runner.h"
#include "lib/output_quote.h"
#include "lib/signal_names.h"
#include "lib/time_parse.h"
#include "lib/args_common.h"

struct bx_timeout_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool foreground;
    bool preserve_status;
    int timeout_signal;
    bool kill_after_specified;
    double kill_after_seconds;
    bool verbose;
    double duration_seconds;
    int first_operand;
};

static volatile sig_atomic_t bx_timeout_pending_signal;

static void bx_timeout_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] DURATION COMMAND [ARG]...\n", progname);
    fprintf(stream, "Start COMMAND, and kill it if still running after DURATION.\n");
    fprintf(stream, "\n");
    fprintf(stream, "DURATION is a floating point number with an optional suffix:\n");
    fprintf(stream, "'s' for seconds (default), 'm' for minutes, 'h' for hours,\n");
    fprintf(stream, "or 'd' for days.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -f, --foreground         allow COMMAND to read from the TTY and receive TTY signals;\n");
    fprintf(stream, "                           in this mode, children of COMMAND are not timed out\n");
    fprintf(stream, "  -s, --signal=SIGNAL     specify the signal to be sent on timeout\n");
    fprintf(stream, "  -k, --kill-after=DURATION  send SIGKILL if command is still running\n");
    fprintf(stream, "  -p, --preserve-status    exit with the same status as COMMAND on timeout\n");
    fprintf(stream, "  -v, --verbose           diagnose to stderr any signal sent upon timeout\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_timeout_parse_duration(const char* text, double* seconds_out) {
    if (seconds_out == NULL) {
        return false;
    }

    const struct bx_time_duration_parse_options parse_options = {
        .allow_infinite = true,
        .require_strtod_range = false,
        .clamp_positive_underflow = true,
    };
    struct bx_time_duration_parse_result result = {
        .seconds = 0.0,
        .infinite = false,
    };
    if (!bx_time_parse_duration_seconds(text, &parse_options, &result)) {
        return false;
    }

    *seconds_out = result.infinite ? DBL_MAX : result.seconds;
    return true;
}

static bool bx_timeout_parse_signal_number(const char* text, int* signal_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    for (const char* p = text; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 255) {
        return false;
    }

    *signal_out = (int)value;
    return true;
}

static bool bx_timeout_parse_signal_name(const char* text, int* signal_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    const char* name = text;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    return bx_signal_name_lookup(name, signal_out);
}

static bool bx_timeout_parse_signal(const char* text, int* signal_out) {
    int signal_number = 0;
    if (!bx_timeout_parse_signal_number(text, &signal_number) &&
        !bx_timeout_parse_signal_name(text, &signal_number)) {
        return false;
    }

    if (signal_number != 0 && sigaction(signal_number, NULL, NULL) != 0 &&
        errno == EINVAL) {
        return false;
    }

    *signal_out = signal_number;
    return true;
}

static bool bx_timeout_parse_options(int argc, char** argv, struct bx_timeout_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"foreground", no_argument, NULL, 'f'},   {"signal", required_argument, NULL, 's'},
        {"kill-after", required_argument, NULL, 'k'},
        {"preserve-status", no_argument, NULL, 'p'},
        {"verbose", no_argument, NULL, 'v'},      {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "timeout");
    options->timeout_signal = SIGTERM;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+:fk:ps:v", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'f':
                options->foreground = true;
                break;
            case 's':
                if (!bx_timeout_parse_signal(optarg, &options->timeout_signal)) {
                    bx_diag(diag, "'%s': invalid signal", optarg != NULL ? optarg : "");
                    return false;
                }
                break;
            case 'k':
                if (!bx_timeout_parse_duration(optarg, &options->kill_after_seconds)) {
                    bx_diag(diag, "invalid time interval '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                options->kill_after_specified = true;
                break;
            case 'p':
                options->preserve_status = true;
                break;
            case 'v':
                options->verbose = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    if (optind >= argc) {
        return false;
    }

    const char* duration_text = argv[optind];
    if (!bx_timeout_parse_duration(duration_text, &options->duration_seconds)) {
        bx_diag(diag, "invalid time interval '%s'", duration_text);
        return false;
    }
    optind++;

    if (optind >= argc) {
        return false;
    }

    options->first_operand = optind;
    return true;
}

static bool bx_timeout_get_monotonic_seconds(double* seconds_out, struct bx_diag_ctx* diag) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        bx_diag(diag, "failed to read monotonic clock: %s", strerror(errno));
        return false;
    }

    if (!bx_time_timespec_to_seconds_double(&now, seconds_out)) {
        bx_diag(diag, "monotonic clock value is invalid");
        return false;
    }
    return true;
}

static bool bx_timeout_sleep_for(double seconds, struct bx_diag_ctx* diag) {
    if (seconds <= 0.0) {
        return true;
    }

    if (seconds > 0.1) {
        seconds = 0.1;
    }

    struct timespec req;
    if (!bx_time_seconds_to_timespec(seconds, &req)) {
        bx_diag(diag, "sleep interval is invalid");
        return false;
    }
    if (req.tv_sec == 0 && req.tv_nsec == 0 &&
        !bx_time_milliseconds_to_timespec(1, &req)) {
        bx_diag(diag, "sleep interval is invalid");
        return false;
    }

    while (nanosleep(&req, &req) != 0) {
        if (errno == EINTR) {
            return true;
        }
        bx_diag(diag, "nanosleep failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static const char* bx_timeout_signal_label(int signal_number) {
    switch (signal_number) {
        case SIGHUP:
            return "HUP";
        case SIGINT:
            return "INT";
        case SIGQUIT:
            return "QUIT";
        case SIGILL:
            return "ILL";
        case SIGABRT:
            return "ABRT";
        case SIGFPE:
            return "FPE";
        case SIGKILL:
            return "KILL";
        case SIGSEGV:
            return "SEGV";
        case SIGPIPE:
            return "PIPE";
        case SIGALRM:
            return "ALRM";
        case SIGTERM:
            return "TERM";
        case SIGUSR1:
            return "USR1";
        case SIGUSR2:
            return "USR2";
        case SIGCHLD:
            return "CHLD";
        case SIGCONT:
            return "CONT";
        case SIGSTOP:
            return "STOP";
        case SIGTSTP:
            return "TSTP";
        case SIGTTIN:
            return "TTIN";
        case SIGTTOU:
            return "TTOU";
#ifdef SIGWINCH
        case SIGWINCH:
            return "WINCH";
#endif
        default:
            return NULL;
    }
}

static void bx_timeout_report_signal(const struct bx_timeout_options* options,
                                     int signal_number,
                                     const char* command) {
    if (!options->verbose) {
        return;
    }

    char* quoted_command = bx_output_quote_dup(command, BX_OUTPUT_QUOTE_LOCALE);
    const char* signal_label = bx_timeout_signal_label(signal_number);
    if (signal_label != NULL) {
        fprintf(stderr, "%s: sending signal %s to command %s\n",
                options->progname, signal_label, quoted_command);
    }
    else {
        fprintf(stderr, "%s: sending signal %d to command %s\n",
                options->progname, signal_number, quoted_command);
    }
    free(quoted_command);
}

static bool bx_timeout_prepare_process_group(const struct bx_timeout_options* options,
                                             struct bx_diag_ctx* diag) {
    if (options->foreground) {
        return true;
    }

    if (bx_child_ensure_current_process_group() == 0) {
        return true;
    }

    bx_diag(diag, "failed to create process group: %s", strerror(errno));
    return false;
}

static bool bx_timeout_send_signal(const struct bx_timeout_options* options,
                                   pid_t child_pid,
                                   const char* command,
                                   int signal_number,
                                   struct bx_diag_ctx* diag) {
    bx_timeout_report_signal(options, signal_number, command);
    if (kill(child_pid, signal_number) != 0 && errno != ESRCH) {
        bx_diag(diag, "failed to signal command: %s", strerror(errno));
        return false;
    }

    if (options->foreground) {
        return true;
    }

    if (bx_child_signal_current_process_group(signal_number, true) != 0 && errno != ESRCH) {
        bx_diag(diag, "failed to signal command group: %s", strerror(errno));
        return false;
    }

    if (signal_number != SIGKILL && signal_number != SIGCONT) {
        (void)kill(child_pid, SIGCONT);
        (void)bx_child_signal_current_process_group(SIGCONT, true);
    }

    return true;
}

static void bx_timeout_signal_handler(int signal_number) {
    if (bx_timeout_pending_signal == 0) {
        bx_timeout_pending_signal = signal_number;
    }
}

static void bx_timeout_sigchld_handler(int signal_number) {
    (void)signal_number;
}

static bool bx_timeout_install_one_handler(int signal_number,
                                           int timeout_signal,
                                           struct bx_diag_ctx* diag) {
    if (signal_number <= 0 || signal_number == SIGKILL || signal_number == SIGSTOP) {
        return true;
    }

    struct sigaction old_action;
    if (sigaction(signal_number, NULL, &old_action) != 0) {
        return errno == EINVAL;
    }
    if (old_action.sa_handler == SIG_IGN &&
        signal_number != SIGALRM && signal_number != timeout_signal) {
        return true;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = bx_timeout_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(signal_number, &action, NULL) != 0) {
        bx_diag(diag, "failed to install signal handler: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_timeout_install_signal_handlers(int timeout_signal,
                                               struct bx_diag_ctx* diag) {
    static const int handled_signals[] = {
        SIGALRM, SIGINT, SIGQUIT, SIGHUP, SIGTERM, SIGPIPE, SIGUSR1, SIGUSR2,
        SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGSEGV,
#ifdef SIGXCPU
        SIGXCPU,
#endif
#ifdef SIGXFSZ
        SIGXFSZ,
#endif
#ifdef SIGSYS
        SIGSYS,
#endif
#ifdef SIGVTALRM
        SIGVTALRM,
#endif
#ifdef SIGPROF
        SIGPROF,
#endif
#ifdef SIGPOLL
        SIGPOLL,
#endif
#ifdef SIGPWR
        SIGPWR,
#endif
#ifdef SIGSTKFLT
        SIGSTKFLT,
#endif
    };

    bx_timeout_pending_signal = 0;
    for (size_t i = 0; i < sizeof(handled_signals) / sizeof(handled_signals[0]); i++) {
        if (!bx_timeout_install_one_handler(handled_signals[i], timeout_signal, diag)) {
            return false;
        }
    }
    if (!bx_timeout_install_one_handler(timeout_signal, timeout_signal, diag)) {
        return false;
    }
#if defined(SIGRTMIN) && defined(SIGRTMAX)
    for (int signal_number = SIGRTMIN; signal_number <= SIGRTMAX; signal_number++) {
        if (!bx_timeout_install_one_handler(signal_number, timeout_signal, diag)) {
            return false;
        }
    }
#endif

    struct sigaction child_action;
    memset(&child_action, 0, sizeof(child_action));
    child_action.sa_handler = bx_timeout_sigchld_handler;
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, NULL) != 0) {
        bx_diag(diag, "failed to install SIGCHLD handler: %s", strerror(errno));
        return false;
    }

    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGALRM);
    sigaddset(&unblock, SIGCHLD);
    if (sigprocmask(SIG_UNBLOCK, &unblock, NULL) != 0) {
        bx_diag(diag, "failed to unblock monitor signals: %s", strerror(errno));
        return false;
    }

    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    return true;
}

struct bx_timeout_reap_result {
    bool reaped;
    int status;
};

static void bx_timeout_reap_status(pid_t pid,
                                   int status,
                                   bool exec_failed,
                                   int exec_errno,
                                   void* user) {
    struct bx_timeout_reap_result* result = user;

    (void)pid;
    (void)exec_failed;
    (void)exec_errno;
    result->reaped = true;
    result->status = status;
}

static bool bx_timeout_wait_for_child(struct bx_child* children,
                                      int* running,
                                      pid_t child_pid,
                                      const char* command,
                                      const struct bx_timeout_options* options,
                                      int* wait_status_out,
                                      bool* timed_out_out,
                                      bool* kill_sent_out,
                                      struct bx_diag_ctx* diag) {
    double now = 0.0;
    if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
        return false;
    }

    bool timeout_enabled = options->duration_seconds > 0.0;
    double deadline = timeout_enabled ? now + options->duration_seconds : DBL_MAX;
    double kill_deadline = 0.0;

    bool timed_out = false;
    bool kill_sent = false;
    bool kill_timer_armed = false;

    while (true) {
        struct bx_timeout_reap_result reap_result = {0};
        if (bx_child_reap(children, running, false, true, bx_timeout_reap_status, &reap_result) != 0) {
            bx_diag(diag, "waitpid failed: %s", strerror(errno));
            return false;
        }
        if (reap_result.reaped) {
            *wait_status_out = reap_result.status;
            *timed_out_out = timed_out;
            *kill_sent_out = kill_sent;
            return true;
        }

        int pending_signal = bx_timeout_pending_signal;
        if (pending_signal != 0) {
            bx_timeout_pending_signal = 0;
            int signal_to_send = pending_signal;
            if (pending_signal == SIGALRM) {
                timed_out = true;
                signal_to_send = options->timeout_signal;
            }
            if (signal_to_send == SIGKILL) {
                kill_sent = true;
            }
            if (options->kill_after_specified && options->kill_after_seconds > 0.0 &&
                !kill_timer_armed && !kill_sent) {
                if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
                    return false;
                }
                kill_deadline = now + options->kill_after_seconds;
                kill_timer_armed = true;
            }
            if (!bx_timeout_send_signal(options, child_pid, command, signal_to_send, diag)) {
                return false;
            }
            continue;
        }

        if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
            return false;
        }

        if (!timed_out && timeout_enabled && now >= deadline) {
            timed_out = true;
            if (options->timeout_signal == SIGKILL) {
                kill_sent = true;
            }
            if (options->kill_after_specified && options->kill_after_seconds > 0.0 &&
                !kill_sent) {
                kill_deadline = now + options->kill_after_seconds;
                kill_timer_armed = true;
            }
            if (!bx_timeout_send_signal(options, child_pid, command,
                                        options->timeout_signal, diag)) {
                return false;
            }
            continue;
        }

        if (kill_timer_armed && !kill_sent && now >= kill_deadline) {
            kill_sent = true;
            if (!bx_timeout_send_signal(options, child_pid, command, SIGKILL, diag)) {
                return false;
            }
            continue;
        }

        double sleep_seconds = 0.1;
        if (!timed_out && timeout_enabled && deadline - now < sleep_seconds) {
            sleep_seconds = deadline - now;
        }
        if (kill_timer_armed && !kill_sent && kill_deadline - now < sleep_seconds) {
            sleep_seconds = kill_deadline - now;
        }
        if (!bx_timeout_sleep_for(sleep_seconds, diag)) {
            return false;
        }
    }
}

static void bx_timeout_force_reap_child(struct bx_child* children, int* running, pid_t child_pid) {
    int saved_errno = errno;
    (void)kill(child_pid, SIGKILL);
    while (*running > 0) {
        if (bx_child_reap(children, running, true, true, NULL, NULL) != 0) {
            break;
        }
    }
    errno = saved_errno;
}

static int bx_timeout_status_from_wait_status(int wait_status) {
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return 128 + WTERMSIG(wait_status);
    }
    return 125;
}

static int bx_timeout_publish_child_status(int wait_status,
                                           bool timed_out,
                                           bool preserve_status,
                                           const char* progname) {
    int status = bx_timeout_status_from_wait_status(wait_status);
    if (timed_out) {
        if (preserve_status || (WIFSIGNALED(wait_status) &&
                               WTERMSIG(wait_status) == SIGKILL)) {
            return status;
        }
        return 124;
    }

    if (WIFSIGNALED(wait_status)) {
        int signal_number = WTERMSIG(wait_status);
#ifdef WCOREDUMP
        if (WCOREDUMP(wait_status)) {
            fprintf(stderr, "%s: the monitored command dumped core\n", progname);
        }
#endif
        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_DFL;
        sigemptyset(&action.sa_mask);
        (void)sigaction(signal_number, &action, NULL);

        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, signal_number);
        (void)sigprocmask(SIG_UNBLOCK, &unblock, NULL);
        (void)raise(signal_number);
    }

    return status;
}

int bx_timeout_main(int argc, char** argv) {
    struct bx_timeout_options options;
    struct bx_diag_ctx diag = {
        .progname = "timeout",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_timeout_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        return 125;
    }

    if (options.show_help) {
        bx_timeout_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (!bx_timeout_prepare_process_group(&options, &diag)) {
        return 125;
    }
    if (!bx_timeout_install_signal_handlers(options.timeout_signal, &diag)) {
        return 125;
    }

    char** command_argv = argv + options.first_operand;
    struct bx_child children[1] = {0};
    int running = 0;
    bool exec_failed_now = false;
    int exec_errno_now = 0;
    struct bx_child_runner_opts runner_opts = bx_child_runner_opts_default();
    runner_opts.reset_tty_stop_signals = true;
    runner_opts.parent_death_signal = options.timeout_signal;
    if (bx_child_spawn_argv(options.progname,
                            command_argv,
                            &runner_opts,
                            0,
                            children,
                            &running,
                            &exec_failed_now,
                            &exec_errno_now) != 0) {
        return 125;
    }
    if (running == 0) {
        bx_diag(&diag, "failed to start command '%s'", command_argv[0]);
        return 125;
    }

    if (exec_failed_now) {
        fprintf(stderr, "%s: failed to run command '%s': %s\n",
                options.progname,
                command_argv[0],
                strerror(exec_errno_now));
        (void)bx_child_reap(children, &running, true, true, NULL, NULL);
        return exec_errno_now == ENOENT ? 127 : 126;
    }

    pid_t child_pid = children[0].pid;
    int wait_status = 0;
    bool timed_out = false;
    bool kill_sent = false;
    if (!bx_timeout_wait_for_child(children, &running, child_pid, command_argv[0],
                                   &options, &wait_status, &timed_out, &kill_sent,
                                   &diag)) {
        bx_timeout_force_reap_child(children, &running, child_pid);
        return 125;
    }

    (void)kill_sent;
    return bx_timeout_publish_child_status(wait_status, timed_out,
                                           options.preserve_status,
                                           options.progname);
}
