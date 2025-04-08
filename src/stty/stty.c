#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

enum bx_stty_special_mode {
    BX_STTY_SPECIAL_NONE = 0,
    BX_STTY_SPECIAL_HELP,
    BX_STTY_SPECIAL_VERSION,
};

static void bx_stty_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [SETTING]...\n", progname);
    fprintf(stream, "Print or change terminal characteristics.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all           print all settings in human-readable form\n");
    fprintf(stream, "  -g, --save          print all settings in stty-readable form\n");
    fprintf(stream, "  -F, --file=DEVICE   use DEVICE instead of standard input\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Most SETTING handling is delegated to the host stty implementation.\n");
}

static void bx_stty_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static enum bx_stty_special_mode bx_stty_detect_special_mode(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            return BX_STTY_SPECIAL_HELP;
        }
        if (strcmp(argv[i], "--version") == 0) {
            return BX_STTY_SPECIAL_VERSION;
        }
    }
    return BX_STTY_SPECIAL_NONE;
}

int bx_stty_main(int argc, char** argv) {
    const enum bx_stty_special_mode special_mode = bx_stty_detect_special_mode(argc, argv);
    if (special_mode == BX_STTY_SPECIAL_HELP) {
        bx_stty_print_help(stdout, "stty");
        return 0;
    }
    if (special_mode == BX_STTY_SPECIAL_VERSION) {
        bx_stty_print_version("stty");
        return 0;
    }

    char** host_argv = xmalloc(((size_t)argc + 1) * sizeof(*host_argv));
    host_argv[0] = (char*)"stty";
    for (int i = 1; i < argc; i++) {
        host_argv[i] = argv[i];
    }
    host_argv[argc] = NULL;

    static const char* const host_candidates[] = {
        "/usr/bin/stty", "/bin/stty", "/usr/sbin/stty", "/sbin/stty", NULL,
    };

    int last_errno = ENOENT;
    for (size_t i = 0; host_candidates[i] != NULL; i++) {
        execv(host_candidates[i], host_argv);
        last_errno = errno;
        if (last_errno != ENOENT && last_errno != ENOTDIR) {
            break;
        }
    }

    struct bx_diag_ctx diag = {
        .progname = "stty",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (last_errno == ENOENT || last_errno == ENOTDIR) {
        bx_diag(&diag, "host stty binary not found");
        free(host_argv);
        return diag.exit_status;
    }

    bx_diag(&diag, "unable to execute host stty: %s", strerror(last_errno));
    free(host_argv);
    return diag.exit_status;
}
