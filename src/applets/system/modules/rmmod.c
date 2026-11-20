#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__linux__) && defined(SYS_delete_module)
/* The kernel's module-name buffer is no larger than 64 bytes. */
enum { MODULE_NAME_CAPACITY = 64 };

static int valid_module_name(const char* name) {
    size_t len = strnlen(name, MODULE_NAME_CAPACITY);
    return name[0] != '-' && len != 0 && len < MODULE_NAME_CAPACITY &&
           strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == len;
}
#endif

int bx_rmmod_main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("usage: rmmod [--] MODULE ...\n"
             "Unload named Linux modules that are not in use.");
        return 0;
    }

    struct bx_diag_ctx diag = {.progname = "rmmod"};
#if defined(__linux__) && defined(SYS_delete_module)
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
        if (!valid_module_name(argv[i])) {
            bx_diag(&diag, "invalid module name: '%s'", argv[i]);
            return 1;
        }
    }

    int status = 0;
    for (int i = operand; i < argc; i++) {
        char name[MODULE_NAME_CAPACITY];
        strcpy(name, argv[i]);
        for (char* p = name; *p != '\0'; p++)
            if (*p == '-')
                *p = '_';

        /* Never ask the kernel to force removal or wait for references. */
        if (syscall(SYS_delete_module, name, (unsigned int)O_NONBLOCK) < 0) {
            if (errno == EWOULDBLOCK)
                bx_diag(&diag, "module '%s' is in use", argv[i]);
            else if (errno == EBUSY)
                bx_diag(&diag, "module '%s' is busy or cannot be unloaded", argv[i]);
            else if (errno == ENOENT)
                bx_diag(&diag, "module '%s' is not loaded", argv[i]);
            else if (errno == ENOSYS) {
                bx_diag(&diag, "the kernel does not support module unloading");
                return 1;
            } else
                bx_perror_path(&diag, argv[i]);
            status = 1;
        }
    }
    return status;
#else
    bx_diag(&diag, "Linux module unloading is not supported on this platform");
    return 1;
#endif
}
