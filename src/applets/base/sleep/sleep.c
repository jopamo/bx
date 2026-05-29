#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

static void bx_sleep_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s NUMBER[SUFFIX]...\n", progname);
    fprintf(stream, "  or:  %s OPTION\n", progname);
    fprintf(stream, "Pause for NUMBER seconds.  SUFFIX may be 's' for seconds (the default),\n");
    fprintf(stream, "'m' for minutes, 'h' for hours or 'd' for days.  NUMBER need not be an\n");
    fprintf(stream, "integer.  Given two or more arguments, pause for the amount of time\n");
    fprintf(stream, "specified by the sum of their values.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
}

static bool bx_sleep_parse_interval(const char* text, double* seconds_out, bool* infinite_out) {
    if (text == NULL || text[0] == '\0' || seconds_out == NULL || infinite_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(text, &end);

    if (end == text || isnan(value)) {
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

    value *= multiplier;
    if (isnan(value) || value < 0.0) {
        return false;
    }

    if (isinf(value)) {
        *seconds_out = 0.0;
        *infinite_out = true;
        return true;
    }

    if (!isfinite(value)) {
        return false;
    }

    *seconds_out = value;
    *infinite_out = false;
    return true;
}

static int bx_sleep_for_seconds(double total_seconds, struct bx_diag_ctx* diag) {
    const double max_chunk_seconds = 1000000000.0;
    const double epsilon = 1e-12;

    while (total_seconds > epsilon) {
        double chunk = total_seconds;
        if (chunk > max_chunk_seconds) {
            chunk = max_chunk_seconds;
        }

        if (chunk < epsilon) {
            break;
        }

        time_t sec_part = (time_t)chunk;
        long nsec_part = (long)((chunk - (double)sec_part) * 1000000000.0);
        if (nsec_part >= 1000000000L) {
            sec_part++;
            nsec_part -= 1000000000L;
        }
        if (nsec_part < 0L) {
            nsec_part = 0L;
        }

        struct timespec ts = {
            .tv_sec = sec_part,
            .tv_nsec = nsec_part,
        };

        while (nanosleep(&ts, &ts) == -1) {
            if (errno != EINTR) {
                bx_perror_path(diag, "nanosleep");
                return 1;
            }
        }

        total_seconds -= chunk;
    }

    return 0;
}

static int bx_sleep_forever(struct bx_diag_ctx* diag) {
    for (;;) {
        int rc = bx_sleep_for_seconds(1000000000.0, diag);
        if (rc != 0) {
            return rc;
        }
    }
}

int bx_sleep_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sleep");
    struct bx_diag_ctx diag = {
        .progname = progname,
        .exit_status = 1,
    };

    bx_args_getopt_reset();

    while (true) {
        int c = bx_args_getopt_long(argc, argv, "", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 1:
                bx_sleep_print_help(stdout, progname);
                return 0;
            case 2:
                bx_cli_print_version(progname);
                return 0;
            case '?':
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
                bx_cli_print_try_help(progname);
                return 1;
            default:
                bx_cli_print_try_help(progname);
                return 1;
        }
    }

    int first_operand = optind;
    if (first_operand >= argc) {
        if (argc == 2 && strcmp(argv[1], "--") == 0) {
            return 0;
        }

        bx_cli_diag_missing_operand(&diag);
        bx_cli_print_try_help(progname);
        return 1;
    }

    double total_seconds = 0.0;
    bool sleep_forever = false;
    for (int i = first_operand; i < argc; i++) {
        double value_seconds = 0.0;
        bool value_infinite = false;
        if (!bx_sleep_parse_interval(argv[i], &value_seconds, &value_infinite)) {
            bx_diag(&diag, "invalid time interval '%s'", argv[i]);
            bx_cli_print_try_help(progname);
            return 1;
        }

        if (value_infinite) {
            sleep_forever = true;
            continue;
        }

        total_seconds += value_seconds;
        if (isinf(total_seconds)) {
            sleep_forever = true;
        }
    }

    if (sleep_forever) {
        return bx_sleep_forever(&diag);
    }

    return bx_sleep_for_seconds(total_seconds, &diag);
}
