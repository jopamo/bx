#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/time_parse.h"
#include "lib/option_scan.h"

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

    const struct bx_time_duration_parse_options parse_options = {
        .allow_infinite = true,
        .require_strtod_range = false,
    };
    struct bx_time_duration_parse_result result = {
        .seconds = 0.0,
        .infinite = false,
    };
    if (!bx_time_parse_duration_seconds(text, &parse_options, &result)) {
        return false;
    }

    *seconds_out = result.seconds;
    *infinite_out = result.infinite;
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

        struct timespec ts;
        if (!bx_time_seconds_to_timespec(chunk, &ts)) {
            bx_diag(diag, "sleep interval is too large");
            return 1;
        }

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
    static const struct bx_option_spec specs[] = {
        {1, 0, "help", BX_OPTION_NO_VALUE},
        {2, 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sleep");
    struct bx_diag_ctx diag = {
        .progname = progname,
        .exit_status = 1,
    };

    double total_seconds = 0.0;
    bool sleep_forever = false;
    bool have_operand = false;
    const char* invalid = NULL;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_MATCH) {
            if (option.spec->id == 1)
                bx_sleep_print_help(stdout, progname);
            else
                bx_cli_print_version(progname);
            return 0;
        }
        if (option.event != BX_OPTION_OPERAND) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            bx_cli_print_try_help(progname);
            return 1;
        }
        have_operand = true;
        if (invalid)
            continue;
        double seconds;
        bool infinite;
        if (!bx_sleep_parse_interval(option.token, &seconds, &infinite)) {
            invalid = option.token;
            continue;
        }
        if (!infinite)
            total_seconds += seconds;
        sleep_forever = sleep_forever || infinite || isinf(total_seconds);
    }

    if (invalid) {
        bx_diag(&diag, "invalid time interval '%s'", invalid);
        bx_cli_print_try_help(progname);
        return 1;
    }
    if (!have_operand) {
        if (argc == 2 && strcmp(argv[1], "--") == 0)
            return 0;
        bx_cli_diag_missing_operand(&diag);
        bx_cli_print_try_help(progname);
        return 1;
    }

    if (sleep_forever) {
        return bx_sleep_forever(&diag);
    }

    return bx_sleep_for_seconds(total_seconds, &diag);
}
