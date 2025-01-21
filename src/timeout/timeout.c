#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

struct bx_timeout_options {
    const char* progname;
    bool show_help;
    bool show_version;
    double duration_seconds;
    int first_operand;
};

static const char* bx_timeout_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "timeout";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_timeout_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] DURATION COMMAND [ARG]...\n", progname);
    fprintf(stream, "Start COMMAND, and kill it if still running after DURATION.\n");
    fprintf(stream, "\n");
    fprintf(stream, "DURATION is a floating point number with an optional suffix:\n");
    fprintf(stream, "'s' for seconds (default), 'm' for minutes, 'h' for hours,\n");
    fprintf(stream, "or 'd' for days.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_timeout_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_timeout_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_timeout_parse_duration(const char* text, double* seconds_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || end == text || value < 0.0 || !isfinite(value)) {
        return false;
    }

    double multiplier = 1.0;
    if (*end != '\0') {
        if (end[1] != '\0') {
            return false;
        }

        switch (*end) {
            case 's':
                multiplier = 1.0;
                break;
            case 'm':
                multiplier = 60.0;
                break;
            case 'h':
                multiplier = 3600.0;
                break;
            case 'd':
                multiplier = 86400.0;
                break;
            default:
                return false;
        }
    }

    double seconds = value * multiplier;
    if (!isfinite(seconds)) {
        return false;
    }

    *seconds_out = seconds;
    return true;
}

static bool bx_timeout_parse_options(int argc, char** argv, struct bx_timeout_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_timeout_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
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

static bool bx_timeout_wait_for_child(pid_t child_pid, double duration_seconds, int* wait_status_out, bool* timed_out_out, struct bx_diag_ctx* diag) {
    double now = 0.0;
    if (!bx_timeout_get_monotonic_seconds(&now, diag)) {
        return false;
    }

    double deadline = now + duration_seconds;
    bool timed_out = false;

    while (true) {
        int status = 0;
        pid_t wait_rc = waitpid(child_pid, &status, WNOHANG);
        if (wait_rc == child_pid) {
            *wait_status_out = status;
            *timed_out_out = timed_out;
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
                if (kill(child_pid, SIGTERM) != 0 && errno != ESRCH) {
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
        bx_timeout_print_try_help(options.progname);
        return 125;
    }

    if (options.show_help) {
        bx_timeout_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_timeout_print_version(options.progname);
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
    if (!bx_timeout_wait_for_child(child_pid, options.duration_seconds, &wait_status, &timed_out, &diag)) {
        bx_timeout_force_reap_child(child_pid);
        return 125;
    }

    if (timed_out) {
        return 124;
    }

    return bx_timeout_status_from_wait_status(wait_status);
}
