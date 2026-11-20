#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include "lib/fd_ops.h"
#include "module_load.h"

int bx_insmod_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "insmod"};
    int operand = 1;
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        puts("usage: insmod [--] MODULE [PARAMETER ...]\n"
             "Load a specified Linux module without resolving dependencies.");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--") == 0)
        operand++;
    else if (argc > 1 && argv[1][0] == '-') {
        bx_diag(&diag, "unsupported option: '%s'", argv[1]);
        return 1;
    }
    if (operand >= argc) {
        bx_diag(&diag, "a module pathname is required");
        return 1;
    }

    const char* path = argv[operand];
    char* parameters = bx_module_parameters(argc - operand - 1, argv + operand + 1);
    if (parameters == NULL) {
        bx_perror_path(&diag, "module parameters");
        return 1;
    }
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    int status = 1;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        bx_perror_path(&diag, path);
    } else if (!S_ISREG(st.st_mode)) {
        bx_diag(&diag, "module is not a regular file: '%s'", path);
    } else {
        if (bx_module_load_fd(fd, &st, parameters) < 0)
            bx_perror_path(&diag, path);
        else
            status = 0;
    }
    bx_fd_cleanup(&fd);
    free(parameters);
    return status;
}
#else
int bx_insmod_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    fputs("insmod: Linux module loading is not supported on this platform\n", stderr);
    return 1;
}
#endif
