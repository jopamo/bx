#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [--] NEW_ROOT PUT_OLD\n"
                 "Move the old root mount to PUT_OLD beneath the new root.\n"
                 "Does not unmount the old root or start a new init.\n", name);
}

int bx_pivot_root_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "pivot_root", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "pivot_root"};
    bool end_options = argc > 1 && strcmp(argv[1], "--") == 0;
    int operand = end_options ? 2 : 1;
    if (argc - operand != 2) {
        usage(stderr, diag.progname);
        return 1;
    }
    const char* new_root = argv[operand];
    const char* put_old = argv[operand + 1];
    if (!end_options && (*new_root == '-' || *put_old == '-')) {
        bx_diag(&diag, "unsupported option: '%s'", *new_root == '-' ? new_root : put_old);
        return 1;
    }
    if (*new_root == '\0' || *put_old == '\0') {
        bx_diag(&diag, "root paths must not be empty");
        return 1;
    }

#if defined(__linux__) && defined(SYS_pivot_root)
    /* The kernel resolves both paths and checks the caller's mount namespace. */
    if (syscall(SYS_pivot_root, new_root, put_old) < 0) {
        bx_diag(&diag, "'%s', '%s': %s", new_root, put_old, bx_strerror(errno));
        return 1;
    }
    return 0;
#else
    bx_diag(&diag, "Linux pivot_root is not supported on this platform");
    return 1;
#endif
}
