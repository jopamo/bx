#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

struct bx_nproc_options {
    const char* progname;
    bool all;
    uintmax_t ignore;
    bool show_help;
    bool show_version;
};

enum bx_nproc_parse_status {
    BX_NPROC_PARSE_OK = 0,
    BX_NPROC_PARSE_ERROR = 1,
    BX_NPROC_PARSE_ERROR_TRY_HELP = 2,
};

static void bx_nproc_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the number of processing units available to the current process,\n");
    fprintf(stream, "which may be less than the number of online processors.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --all      print the number of installed processors\n");
    fprintf(stream, "      --ignore=N  if possible, exclude N processors\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static bool bx_nproc_parse_ignore(const char* text, uintmax_t* value_out, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        bx_diag(diag, "invalid number: '%s'", text == NULL ? "" : text);
        return false;
    }

    const char* digits = text;
    while (isspace((unsigned char)*digits)) {
        digits++;
    }
    if (digits[0] == '+') {
        digits++;
    }
    else if (digits[0] == '-') {
        bx_diag(diag, "invalid number: '%s'", text);
        return false;
    }

    if (digits[0] == '\0') {
        bx_diag(diag, "invalid number: '%s'", text);
        return false;
    }

    uintmax_t value = 0;
    bool saturated = false;
    for (const char* p = digits; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            bx_diag(diag, "invalid number: '%s'", text);
            return false;
        }

        if (saturated) {
            continue;
        }

        unsigned int digit = (unsigned int)(*p - '0');
        if (value > (UINTMAX_MAX - digit) / 10u) {
            value = UINTMAX_MAX;
            saturated = true;
            continue;
        }
        value = (value * 10u) + (uintmax_t)digit;
    }

    *value_out = value;
    return true;
}

static enum bx_nproc_parse_status bx_nproc_parse_options(
    int argc,
    char** argv,
    struct bx_nproc_options* options,
    struct bx_diag_ctx* diag
) {
    static const struct bx_option_spec specs[] = {
        {1, 0, "all", BX_OPTION_NO_VALUE},
        {2, 0, "ignore", BX_OPTION_REQUIRED_VALUE},
        {3, 0, "help", BX_OPTION_NO_VALUE},
        {4, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "nproc");
    diag->progname = options->progname;

    const char* extra = NULL;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!extra)
                extra = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option '%s' requires an argument", option.token);
            else if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return BX_NPROC_PARSE_ERROR_TRY_HELP;
        }
        switch (option.spec->id) {
            case 1:
                options->all = true;
                break;
            case 2:
                if (!bx_nproc_parse_ignore(option.value, &options->ignore, diag)) {
                    return BX_NPROC_PARSE_ERROR;
                }
                break;
            case 3:
                options->show_help = true;
                return BX_NPROC_PARSE_OK;
            case 4:
                options->show_version = true;
                return BX_NPROC_PARSE_OK;
            default:
                return BX_NPROC_PARSE_ERROR;
        }
    }

    if (extra) {
        bx_cli_diag_extra_operand(diag, extra);
        return BX_NPROC_PARSE_ERROR_TRY_HELP;
    }

    return BX_NPROC_PARSE_OK;
}

static long bx_nproc_available(void) {
#ifdef __linux__
    cpu_set_t local;
    cpu_set_t* mask = &local;
    size_t bytes = sizeof(local);
    for (;;) {
        int rc = sched_getaffinity(0, bytes, mask);
        int err = errno;
        long count = rc == 0 ? CPU_COUNT_S(bytes, mask) : 0;
        if (mask != &local)
            free(mask);
        if (count > 0)
            return count;
        if (rc == 0 || err != EINVAL || bytes >= 1024 * 1024)
            break;
        bytes *= 2;
        mask = malloc(bytes);
        if (!mask)
            break;
    }
#endif
    return sysconf(_SC_NPROCESSORS_ONLN);
}

int bx_nproc_main(int argc, char** argv) {
    struct bx_nproc_options options;
    struct bx_diag_ctx diag = {.progname = "nproc", .exit_status = 0};
    enum bx_nproc_parse_status parse_status = bx_nproc_parse_options(argc, argv, &options, &diag);

    if (parse_status != BX_NPROC_PARSE_OK) {
        if (parse_status == BX_NPROC_PARSE_ERROR_TRY_HELP) {
            bx_cli_print_try_help(options.progname);
        }
        return 1;
    }
    if (options.show_help) {
        bx_nproc_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    long nproc;
    if (options.all) {
        nproc = sysconf(_SC_NPROCESSORS_CONF);
    }
    else {
        nproc = bx_nproc_available();
    }

    if (nproc <= 0)
        nproc = 1;

    if (options.ignore >= (uintmax_t)nproc) {
        nproc = 1;
    }
    else {
        nproc -= (long)options.ignore;
    }
    if (nproc <= 0) {
        nproc = 1;
    }

    printf("%ld\n", nproc);

    return 0;
}
