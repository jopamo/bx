#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/time_parse.h"
#include "lib/args_common.h"

struct bx_timeout_options {
    const char* progname;
    bool show_help;
    bool show_version;
    int timeout_signal;
    bool kill_after_specified;
    double kill_after_seconds;
    bool verbose;
    double duration_seconds;
    int first_operand;
};

static void bx_timeout_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] DURATION COMMAND [ARG]...\n", progname);
    fprintf(stream, "Start COMMAND, and kill it if still running after DURATION.\n");
    fprintf(stream, "\n");
    fprintf(stream, "DURATION is a floating point number with an optional suffix:\n");
    fprintf(stream, "'s' for seconds (default), 'm' for minutes, 'h' for hours,\n");
    fprintf(stream, "or 'd' for days.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --signal=SIGNAL     specify the signal to be sent on timeout\n");
    fprintf(stream, "  -k, --kill-after=DURATION  send SIGKILL if command is still running\n");
    fprintf(stream, "  -v, --verbose           diagnose to stderr any signal sent upon timeout\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_timeout_parse_duration(const char* text, double* seconds_out) {
    if (seconds_out == NULL) {
        return false;
    }

    struct bx_time_duration_parse_result result = {
        .seconds = 0.0,
        .infinite = false,
    };
    if (!bx_time_parse_duration_seconds(text, NULL, &result)) {
        return false;
    }

    *seconds_out = result.seconds;
    return true;
}

struct bx_timeout_signal_name {
    const char* name;
    int value;
};

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
    if (errno != 0 || end == text || *end != '\0' || value <= 0 || value > 255) {
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

    static const struct bx_timeout_signal_name known_signals[] = {
        {"HUP", SIGHUP},       {"INT", SIGINT},   {"QUIT", SIGQUIT}, {"ILL", SIGILL},   {"ABRT", SIGABRT}, {"FPE", SIGFPE},   {"KILL", SIGKILL},
        {"SEGV", SIGSEGV},     {"PIPE", SIGPIPE}, {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"CHLD", SIGCHLD},
        {"CONT", SIGCONT},     {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
#ifdef SIGBUS
        {"BUS", SIGBUS},
#endif
#ifdef SIGPOLL
        {"POLL", SIGPOLL},
#endif
#ifdef SIGPROF
        {"PROF", SIGPROF},
#endif
#ifdef SIGSYS
        {"SYS", SIGSYS},
#endif
#ifdef SIGTRAP
        {"TRAP", SIGTRAP},
#endif
#ifdef SIGURG
        {"URG", SIGURG},
#endif
#ifdef SIGVTALRM
        {"VTALRM", SIGVTALRM},
#endif
#ifdef SIGXCPU
        {"XCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
        {"XFSZ", SIGXFSZ},
#endif
#ifdef SIGWINCH
        {"WINCH", SIGWINCH},
#endif
    };

    for (size_t i = 0; i < sizeof(known_signals) / sizeof(known_signals[0]); i++) {
        if (strcasecmp(name, known_signals[i].name) == 0) {
            *signal_out = known_signals[i].value;
            return true;
        }
    }

    return false;
}

static bool bx_timeout_parse_signal(const char* text, int* signal_out) {
    if (bx_timeout_parse_signal_number(text, signal_out)) {
        return true;
    }
    return bx_timeout_parse_signal_name(text, signal_out);
}

static bool bx_timeout_parse_options(int argc, char** argv, struct bx_timeout_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"signal", required_argument, NULL, 's'}, {"kill-after", required_argument, NULL, 'k'},
        {"verbose", no_argument, NULL, 'v'},      {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "timeout");
    options->timeout_signal = SIGTERM;
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "+:s:k:v", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 's':
                if (!bx_timeout_parse_signal(optarg, &options->timeout_signal)) {
                    bx_diag(diag, "invalid signal '%s'", optarg != NULL ? optarg : "");
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
        bx_diag(diag, "missing operand");
        return false;
    }

    const char* duration_text = argv[optind];
    if (!bx_timeout_parse_duration(duration_text, &options->duration_seconds)) {
        bx_diag(diag, "invalid time interval '%s'", duration_text);
        return false;
    }
    optind++;

    if (optind >= argc) {
        bx_diag(diag, "missing operand");
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

    *seconds_out = (double)now.tv_sec + ((double)now.tv_nsec / 1000000000.0);
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
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1000000000.0);
    if (req.tv_nsec >= 1000000000L) {
        req.tv_sec += 1;
        req.tv_nsec -= 1000000000L;
    }
    if (req.tv_sec == 0 && req.tv_nsec == 0) {
        req.tv_nsec = 1000000L;
    }

    while (nanosleep(&req, &req) != 0) {
        if (errno != EINTR) {
            bx_diag(diag, "nanosleep failed: %s", strerror(errno));
            return false;
        }
    }

    return true;
}

static int bx_timeout_exec_command(char** command_argv, const char* progname) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    fprintf(stderr, "%s: failed to run command '%s': %s\n", progname, command_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
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

static void bx_timeout_report_signal(const struct bx_timeout_options* options, int signal_number, pid_t child_pid) {
    if (!options->verbose) {
        return;
    }

    const char* signal_label = bx_timeout_signal_label(signal_number);
    if (signal_label != NULL) {
        fprintf(stderr, "%s: sending signal %s to command %ld\n", options->progname, signal_label, (long)child_pid);
    }
    else {
        fprintf(stderr, "%s: sending signal %d to command %ld\n", options->progname, signal_number, (long)child_pid);
    }
}

static bool bx_timeout_wait_for_child(pid_t child_pid, const struct bx_timeout_options* options, int* wait_status_out, bool* timed_out_out, bool* kill_sent_out, struct bx_diag_ctx* diag) {
    double now = 0.0;
    if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
        return false;
    }

    double deadline = now + options->duration_seconds;
    double kill_deadline = 0.0;
    if (options->kill_after_specified) {
        kill_deadline = deadline + options->kill_after_seconds;
    }

    bool timed_out = false;
    bool kill_sent = false;

    while (true) {
        int status = 0;
        pid_t wait_rc = waitpid(child_pid, &status, WNOHANG);
        if (wait_rc == child_pid) {
            *wait_status_out = status;
            *timed_out_out = timed_out;
            *kill_sent_out = kill_sent;
            return true;
        }
        if (wait_rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            bx_diag(diag, "waitpid failed: %s", strerror(errno));
            return false;
        }

        if (!timed_out) {
            if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
                return false;
            }

            if (now >= deadline) {
                timed_out = true;
                bx_timeout_report_signal(options, options->timeout_signal, child_pid);
                if (options->timeout_signal == SIGKILL) {
                    kill_sent = true;
                }
                if (kill(child_pid, options->timeout_signal) != 0 && errno != ESRCH) {
                    bx_diag(diag, "failed to signal command: %s", strerror(errno));
                    return false;
                }
                continue;
            }

            if (!bx_timeout_sleep_for(deadline - now, diag)) {
                return false;
            }
        }
        else {
            if (options->kill_after_specified && !kill_sent) {
                if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
                    return false;
                }

                if (now >= kill_deadline) {
                    kill_sent = true;
                    bx_timeout_report_signal(options, SIGKILL, child_pid);
                    if (kill(child_pid, SIGKILL) != 0 && errno != ESRCH) {
                        bx_diag(diag, "failed to signal command: %s", strerror(errno));
                        return false;
                    }
                    continue;
                }
            }

            if (!bx_timeout_sleep_for(0.05, diag)) {
                return false;
            }
        }
    }
}

static void bx_timeout_force_reap_child(pid_t child_pid) {
    int saved_errno = errno;
    (void)kill(child_pid, SIGKILL);
    while (waitpid(child_pid, NULL, 0) < 0) {
        if (errno != EINTR) {
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

    char** command_argv = argv + options.first_operand;
    pid_t child_pid = fork();
    if (child_pid < 0) {
        bx_diag(&diag, "failed to fork: %s", strerror(errno));
        return 125;
    }

    if (child_pid == 0) {
        int exec_status = bx_timeout_exec_command(command_argv, options.progname);
        _exit(exec_status);
    }

    int wait_status = 0;
    bool timed_out = false;
    bool kill_sent = false;
    if (!bx_timeout_wait_for_child(child_pid, &options, &wait_status, &timed_out, &kill_sent, &diag)) {
        bx_timeout_force_reap_child(child_pid);
        return 125;
    }

    if (timed_out) {
        if (kill_sent) {
            return 128 + SIGKILL;
        }
        return 124;
    }

    return bx_timeout_status_from_wait_status(wait_status);
}
