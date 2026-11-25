#define _GNU_SOURCE

#include <errno.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

struct bx_reboot_options {
    const char* progname;
    const char* action_text;
    int action_command;
    int init_signal;
    bool force;
    bool show_help;
    bool show_version;
};

static void bx_reboot_set_action(struct bx_reboot_options* options) {
    options->action_text = "reboot";
    options->action_command = RB_AUTOBOOT;
    options->init_signal = SIGTERM;

    if (strcmp(options->progname, "halt") == 0) {
        options->action_text = "halt";
        options->action_command = RB_HALT_SYSTEM;
        options->init_signal = SIGUSR1;
    }
    else if (strcmp(options->progname, "poweroff") == 0) {
        options->action_text = "power off";
        options->action_command = RB_POWER_OFF;
        options->init_signal = SIGUSR2;
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
    fprintf(stream, "  -f, --force    bypass init and request the kernel action directly\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
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

    if (!options->force) {
        /* Signaling init must not grant shutdown to callers without CAP_SYS_BOOT. */
        struct __user_cap_header_struct header = {.version = _LINUX_CAPABILITY_VERSION_3};
        struct __user_cap_data_struct caps[_LINUX_CAPABILITY_U32S_3];
        if (syscall(SYS_capget, &header, caps) < 0)
            return -1;
        if (!(caps[CAP_TO_INDEX(CAP_SYS_BOOT)].effective & CAP_TO_MASK(CAP_SYS_BOOT))) {
            errno = EPERM;
            return -1;
        }
        return kill(1, options->init_signal);
    }
    sync();
    return reboot(options->action_command);
}

static bool bx_reboot_parse_options(int argc, char** argv, struct bx_reboot_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'f', 'f', "force", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "reboot");
    bx_reboot_set_action(options);
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END) {
            if (result.index >= (size_t)argc)
                return true;
            bx_cli_diag_extra_operand(diag, argv[result.index]);
            return false;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.short_name)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        switch (result.spec->id) {
            case 'f':
                options->force = true;
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
        }
    }
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
        bx_cli_print_try_help(options.progname);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_reboot_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (bx_reboot_perform(&options) != 0) {
        bx_diag(&diag, "failed to %s: %s", options.action_text, strerror(errno));
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    return 0;
}
