#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "applets.h"
#include "diag.h"

struct bx_kill_options {
    const char* progname;
    bool show_help;
    bool show_version;
    int signal_number;
    int first_pid_index;
};

struct bx_kill_signal_name {
    const char* name;
    int value;
};

static const char* bx_kill_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "kill";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_kill_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... PID...\n", progname);
    fprintf(stream, "Send a signal to one or more process IDs.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -s, --signal=SIGNAL  specify signal by name (TERM, SIGTERM) or number\n");
    fprintf(stream, "  -h, --help           display this help and exit\n");
    fprintf(stream, "  -V, --version        output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase supports positive PID operands only.\n");
}

static void bx_kill_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_kill_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_kill_parse_signal_number(const char* text, int* signal_out) {
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
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > 255) {
        return false;
    }

    *signal_out = (int)value;
    return true;
}

static bool bx_kill_parse_signal_name(const char* text, int* signal_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    const char* name = text;
    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    static const struct bx_kill_signal_name known_signals[] = {
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

static bool bx_kill_parse_signal(const char* text, int* signal_out) {
    if (bx_kill_parse_signal_number(text, signal_out)) {
        return true;
    }
    return bx_kill_parse_signal_name(text, signal_out);
}

static bool bx_kill_parse_options(int argc, char** argv, struct bx_kill_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"signal", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_kill_progname((argc > 0) ? argv[0] : NULL);
    options->signal_number = SIGTERM;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, ":+s:hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 's':
                if (!bx_kill_parse_signal(optarg, &options->signal_number)) {
                    bx_diag(diag, "invalid signal '%s'", optarg != NULL ? optarg : "");
                    return false;
                }
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
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
            default:
                return false;
        }
    }

    options->first_pid_index = optind;
    return true;
}

static bool bx_kill_parse_pid(const char* text, pid_t* pid_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0) {
        return false;
    }

    pid_t pid = (pid_t)value;
    if ((long)pid != value) {
        return false;
    }

    *pid_out = pid;
    return true;
}

static bool bx_kill_signal_pids(const struct bx_kill_options* options, int argc, char** argv, struct bx_diag_ctx* diag) {
    if (options->first_pid_index >= argc) {
        bx_diag(diag, "missing PID operand");
        return false;
    }

    bool had_error = false;
    for (int i = options->first_pid_index; i < argc; i++) {
        pid_t pid = -1;
        const char* pid_text = argv[i];
        if (!bx_kill_parse_pid(pid_text, &pid)) {
            bx_diag(diag, "invalid PID '%s'", pid_text != NULL ? pid_text : "");
            had_error = true;
            continue;
        }

        if (kill(pid, options->signal_number) != 0) {
            bx_diag(diag, "failed to signal PID %ld: %s", (long)pid, strerror(errno));
            had_error = true;
        }
    }

    return !had_error;
}

int bx_kill_main(int argc, char** argv) {
    struct bx_kill_options options;
    struct bx_diag_ctx diag = {
        .progname = "kill",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_kill_parse_options(argc, argv, &options, &diag)) {
        bx_kill_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_kill_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_kill_print_version(options.progname);
        return 0;
    }

    if (!bx_kill_signal_pids(&options, argc, argv, &diag)) {
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
