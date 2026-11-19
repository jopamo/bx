#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <stdint.h>
#include <sys/syscall.h>

#include "lib/fd_ops.h"
#include "lib/xreadwrite.h"

/* Stable Linux finit_module flag; older build headers may not define it. */
enum { BX_MODULE_INIT_COMPRESSED_FILE = 4 };

static int module_flags(int fd) {
    unsigned char header[6];
    size_t done = 0;
    while (done < sizeof(header)) {
        ssize_t n = pread(fd, header + done, sizeof(header) - done, (off_t)done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0)
            break;
        done += (size_t)n;
    }
    if (done == 0) {
        errno = ENOEXEC;
        return -1;
    }
    if ((done >= 2 && memcmp(header, "\x1f\x8b", 2) == 0) ||
        (done >= 4 && memcmp(header, "\x28\xb5\x2f\xfd", 4) == 0) ||
        (done == 6 && memcmp(header, "\xfd" "7zXZ\0", 6) == 0))
        return BX_MODULE_INIT_COMPRESSED_FILE;
    return 0;
}

static int load_module(int fd, const struct stat* st, const char* parameters, int flags) {
#ifdef SYS_finit_module
    if (syscall(SYS_finit_module, fd, parameters, flags) == 0)
        return 0;
    /* Do not retry a load that failed validation or authorization. */
    if (errno != ENOSYS)
        return -1;
#endif
#ifdef SYS_init_module
    if (flags == 0) {
        if (st->st_size <= 0 || (uintmax_t)st->st_size > 128 * UINTMAX_C(1024) * 1024) {
            errno = st->st_size <= 0 ? ENOEXEC : EFBIG;
            return -1;
        }
        size_t length = (size_t)st->st_size;
        unsigned char* data = malloc(length);
        if (data == NULL)
            return -1;
        size_t done = 0;
        while (done < length) {
            ssize_t n = bx_xread(fd, data + done, length - done);
            if (n <= 0) {
                int error = n == 0 ? ENOEXEC : errno;
                free(data);
                errno = error;
                return -1;
            }
            done += (size_t)n;
        }
        int rc = (int)syscall(SYS_init_module, data, (unsigned long)length, parameters);
        int error = errno;
        free(data);
        errno = error;
        return rc;
    }
#else
    (void)fd;
    (void)st;
    (void)parameters;
    (void)flags;
#endif
    errno = ENOSYS;
    return -1;
}

static char* module_parameters(int argc, char** argv) {
    size_t length = 1;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (n >= SIZE_MAX - length) {
            errno = EOVERFLOW;
            return NULL;
        }
        length += n + 1;
    }
    char* parameters = malloc(length);
    if (parameters == NULL)
        return NULL;
    char* end = parameters;
    for (int i = 0; i < argc; i++) {
        if (i != 0)
            *end++ = ' ';
        size_t n = strlen(argv[i]);
        memcpy(end, argv[i], n);
        end += n;
    }
    *end = '\0';
    return parameters;
}

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
    char* parameters = module_parameters(argc - operand - 1, argv + operand + 1);
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
        int flags = module_flags(fd);
        if (flags < 0 || load_module(fd, &st, parameters, flags) < 0)
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
