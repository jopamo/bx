#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <errno.h>
#include "module_load.h"
#endif

int bx_rmmod_main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("usage: rmmod [--] MODULE ...\n"
             "Unload named Linux modules that are not in use.");
        return 0;
    }

    struct bx_diag_ctx diag = {.progname = "rmmod"};
#ifdef __linux__
    int operand = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    if (operand >= argc) {
        bx_diag(&diag, "a module name is required");
        return 1;
    }
    /* Validate the whole command before issuing the first unload. */
    for (int i = operand; i < argc; i++) {
        if (operand == 1 && argv[i][0] == '-') {
            bx_diag(&diag, "unsupported option: '%s'", argv[i]);
            return 1;
        }
        if (!bx_module_remove_name_valid((struct bx_module_path) {argv[i], strlen(argv[i])})) {
            bx_diag(&diag, "invalid module name: '%s'", argv[i]);
            return 1;
        }
    }

    int status = 0;
    for (int i = operand; i < argc; i++) {
        struct bx_module_path name = {argv[i], strlen(argv[i])};
        if (bx_module_remove(name) < 0) {
            int error = errno;
            bx_module_remove_report(&diag, name, error);
            if (error == ENOSYS)
                return 1;
            status = 1;
        }
    }
    return status;
#else
    bx_diag(&diag, "Linux module unloading is not supported on this platform");
    return 1;
#endif
}
