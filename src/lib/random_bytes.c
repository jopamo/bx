#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/random.h>
#endif

#include "lib/fd_ops.h"
#include "lib/random_bytes.h"

static bool bx_random_bytes_from_urandom(void* buffer, size_t length) {
    unsigned char* out = buffer;
    size_t offset = 0;
    int fd = bx_fd_open_cloexec("/dev/urandom", O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    while (offset < length) {
        ssize_t n = read(fd, out + offset, length - offset);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }

        int saved_errno = (n == 0) ? EIO : errno;
        bx_fd_cleanup(&fd);
        errno = saved_errno;
        return false;
    }

    return bx_fd_close(&fd, NULL, NULL);
}

bool bx_random_bytes(void* buffer, size_t length) {
    if (buffer == NULL && length > 0) {
        errno = EINVAL;
        return false;
    }

    if (length == 0) {
        return true;
    }

#ifdef __linux__
    unsigned char* out = buffer;
    size_t offset = 0;
    while (offset < length) {
        ssize_t n = getrandom(out + offset, length - offset, 0);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno == ENOSYS && offset == 0) {
            return bx_random_bytes_from_urandom(buffer, length);
        }
        return false;
    }

    return true;
#else
    return bx_random_bytes_from_urandom(buffer, length);
#endif
}
