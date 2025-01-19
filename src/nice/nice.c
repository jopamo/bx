#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

struct bx_nice_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool adjustment_specified;
    int adjustment;
    int first_operand;
};

static const char* bx_nice_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "nice";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_nice_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] [COMMAND [ARG]...]\n", progname);
    fprintf(stream, "Run COMMAND with an adjusted scheduling priority.\n");
    fprintf(stream, "With no COMMAND, print the current niceness.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -n, --adjustment=N  add integer N to niceness (default 10)\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

static void bx_nice_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_nice_parse_int(const char* text, int* value_out) {
    if (text == NULL || text[0] == '\0') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return false;
    }

    *value_out = (int)value;
    return true;
}

static bool bx_nice_parse_legacy_adjustment(const char* arg, int* adjustment_out) {
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0' || arg[1] == '-') {
        return false;
    }

    for (const char* p = arg + 1; *p != '\0'; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }

    return bx_nice_parse_int(arg + 1, adjustment_out);
}

static bool bx_nice_parse_options(int argc, char** argv, struct bx_nice_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"adjustment", required_argument, NULL, 'n'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_nice_progname((argc > 0) ? argv[0] : NULL);
    options->adjustment = 10;
    diag->progname = options->progname;

    int option_start = 1;
    if (argc > 1) {
        int legacy_adjustment = 0;
        if (bx_nice_parse_legacy_adjustment(argv[1], &legacy_adjustment)) {
            options->adjustment_specified = true;
            options->adjustment = legacy_adjustment;
            option_start = 2;
        }
    }

    opterr = 0;
    optind = option_start;

    while (true) {
        int c = getopt_long(argc, argv, "+n:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'n': {
                int adjustment = 0;
                if (!bx_nice_parse_int(optarg, &adjustment)) {
                    bx_diag(diag, "invalid adjustment '%s'", optarg);
                    return false;
                }
                options->adjustment_specified = true;
                options->adjustment = adjustment;
                break;
            }
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                if (optopt == 'n') {
                    bx_diag(diag, "option requires an argument -- 'n'");
                }
                else if (optopt != 0) {
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

    options->first_operand = optind;
    return true;
}

static bool bx_nice_print_current_niceness(struct bx_diag_ctx* diag) {
    errno = 0;
    int value = getpriority(PRIO_PROCESS, 0);
    if (value == -1 && errno != 0) {
        bx_diag(diag, "failed to get niceness: %s", strerror(errno));
        return false;
    }

    if (printf("%d\n", value) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static void bx_nice_apply_adjustment(int adjustment, struct bx_diag_ctx* diag) {
    errno = 0;
    int current = getpriority(PRIO_PROCESS, 0);
    if (current == -1 && errno != 0) {
        bx_diag(diag, "failed to get current niceness: %s", strerror(errno));
        return;
    }

    long target = (long)current + (long)adjustment;
    if (target < INT_MIN) {
        target = INT_MIN;
    }
    if (target > INT_MAX) {
        target = INT_MAX;
    }

    if (setpriority(PRIO_PROCESS, 0, (int)target) != 0) {
        bx_diag(diag, "failed to adjust niceness by %d: %s", adjustment, strerror(errno));
    }
}

static int bx_nice_exec_command(char** command_argv, struct bx_diag_ctx* diag) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "%s: %s", command_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_nice_main(int argc, char** argv) {
    struct bx_nice_options options;
    struct bx_diag_ctx diag = {
        .progname = "nice",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_nice_parse_options(argc, argv, &options, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_nice_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_nice_print_version(options.progname);
        return 0;
    }

    if (options.first_operand >= argc) {
        if (options.adjustment_specified) {
            bx_diag(&diag, "a command is required when an adjustment is specified");
            return diag.exit_status;
        }

        if (!bx_nice_print_current_niceness(&diag)) {
            return diag.exit_status;
        }
        return 0;
    }

    bx_nice_apply_adjustment(options.adjustment, &diag);
    return bx_nice_exec_command(argv + options.first_operand, &diag);
}
