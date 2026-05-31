#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#include "lib/xreadwrite.h"

ssize_t bx_xread(int fd, void* buffer, size_t count) {
    while (true) {
        ssize_t nread = read(fd, buffer, count);
        if (nread < 0 && errno == EINTR) {
            continue;
        }
        return nread;
    }
}

enum bx_xwrite_status bx_xwrite_all_status(int fd, const void* buffer, size_t count) {
    const unsigned char* p = (const unsigned char*)buffer;
    size_t done = 0;

    while (done < count) {
        ssize_t nwritten = write(fd, p + done, count - done);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return BX_XWRITE_ERROR;
        }
        if (nwritten == 0) {
            errno = EIO;
            return BX_XWRITE_ZERO;
        }
        done += (size_t)nwritten;
    }

    return BX_XWRITE_OK;
}

bool bx_xwrite_all(int fd, const void* buffer, size_t count) {
    return bx_xwrite_all_status(fd, buffer, count) == BX_XWRITE_OK;
}
