#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"

struct bx_reboot_options {
    const char* progname;
    const char* action_text;
    int action_command;
    bool show_help;
    bool show_version;
};

static const char* bx_reboot_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "reboot";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_reboot_set_action(struct bx_reboot_options* options) {
    options->action_text = "reboot";
    options->action_command = RB_AUTOBOOT;

    if (strcmp(options->progname, "halt") == 0) {
        options->action_text = "halt";
        options->action_command = RB_HALT_SYSTEM;
    }
    else if (strcmp(options->progname, "poweroff") == 0) {
        options->action_text = "power off";
        options->action_command = RB_POWER_OFF;
    }
}

static void bx_reboot_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    if (strcmp(progname, "halt") == 0) {
        fprintf(stream, "Request a system halt.\n");
    }
    else if (strcmp(progname, "poweroff") == 0) {
        fprintf(stream, "Request a system power off.\n");
    }
    else {
        fprintf(stream, "Request a system reboot.\n");
    }
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static void bx_reboot_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_reboot_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

enum bx_reboot_backend_kind {
    BX_REBOOT_BACKEND_SYSTEM = 0,
    BX_REBOOT_BACKEND_MOCK,
    BX_REBOOT_BACKEND_UNSUPPORTED,
};

static enum bx_reboot_backend_kind bx_reboot_backend_kind(void) {
    const char* backend = getenv("BX_REBOOT_BACKEND");
    if (backend == NULL || backend[0] == '\0') {
        return BX_REBOOT_BACKEND_SYSTEM;
    }

    if (strcmp(backend, "mock") == 0) {
        return BX_REBOOT_BACKEND_MOCK;
    }

    if (strcmp(backend, "unsupported") == 0) {
        return BX_REBOOT_BACKEND_UNSUPPORTED;
    }

    return BX_REBOOT_BACKEND_UNSUPPORTED;
}

static int bx_reboot_perform(const struct bx_reboot_options* options) {
    switch (bx_reboot_backend_kind()) {
        case BX_REBOOT_BACKEND_MOCK:
            errno = EPERM;
            return -1;
        case BX_REBOOT_BACKEND_UNSUPPORTED:
            errno = ENOSYS;
            return -1;
        case BX_REBOOT_BACKEND_SYSTEM:
        default:
            break;
    }

    sync();
    return reboot(options->action_command);
}

static bool bx_reboot_parse_options(int argc, char** argv, struct bx_reboot_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_reboot_progname((argc > 0) ? argv[0] : NULL);
    bx_reboot_set_action(options);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
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
            default:
                return false;
        }
    }

    if (optind < argc) {
        bx_diag(diag, "extra operand '%s'", argv[optind]);
        return false;
    }

    return true;
}

int bx_reboot_main(int argc, char** argv) {
    struct bx_reboot_options options;
    struct bx_diag_ctx diag = {
        .progname = "reboot",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_reboot_parse_options(argc, argv, &options, &diag)) {
        bx_reboot_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_reboot_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_reboot_print_version(options.progname);
        return 0;
    }

    if (bx_reboot_perform(&options) != 0) {
        bx_diag(&diag, "failed to %s: %s", options.action_text, strerror(errno));
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
