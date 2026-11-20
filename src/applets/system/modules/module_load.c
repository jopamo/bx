#define _GNU_SOURCE
#include "module_load.h"

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

char* bx_module_parameters(int argc, char** argv) {
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

int bx_module_load_fd(int fd, const struct stat* st, const char* parameters) {
    int flags = module_flags(fd);
    return flags < 0 ? -1 : load_module(fd, st, parameters, flags);
}

/* The kernel's module-name buffer is no larger than 64 bytes. */
enum { MODULE_NAME_CAPACITY = 64 };

bool bx_module_remove_name_valid(struct bx_module_path name) {
    return name.len < MODULE_NAME_CAPACITY && bx_module_name_span_valid(name.text, name.len) &&
           name.text[0] != '-';
}

int bx_module_remove(struct bx_module_path name) {
    if (!bx_module_remove_name_valid(name)) {
        errno = EINVAL;
        return -1;
    }
#ifdef SYS_delete_module
    char normalized[MODULE_NAME_CAPACITY];
    for (size_t i = 0; i < name.len; i++)
        normalized[i] = name.text[i] == '-' ? '_' : name.text[i];
    normalized[name.len] = '\0';
    return (int)syscall(SYS_delete_module, normalized, (unsigned int)O_NONBLOCK);
#else
    errno = ENOSYS;
    return -1;
#endif
}

void bx_module_remove_report(struct bx_diag_ctx* diag, struct bx_module_path name, int error) {
    if (error == ENOSYS) {
        bx_diag(diag, "the kernel does not support module unloading");
        return;
    }
    const char* reason = error == EWOULDBLOCK ? "is in use" :
                         error == EBUSY ? "is busy or cannot be unloaded" :
                         error == ENOENT ? "is not loaded" : NULL;
    if (reason != NULL)
        bx_diag(diag, "module '%.*s' %s", (int)name.len, name.text, reason);
    else
        bx_diag(diag, "cannot unload '%.*s': %s", (int)name.len, name.text, strerror(error));
}
#endif
