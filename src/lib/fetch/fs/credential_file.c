#define _GNU_SOURCE
#include "lib/fetch/credential_file.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/secure_path.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int bx_fetch_bearer_token_load_file(const char* path, char** token) {
    if (!path || !path[0] || !token) {
        errno = EINVAL;
        return -1;
    }
    char* basename = NULL;
    int parent = bx_fetch_secure_path_open_parent_directory(path, false, &basename);
    if (parent < 0)
        return -1;
    /* Nonblocking open prevents an untrusted FIFO from hanging before fstat. */
    int fd = bx_fetch_secure_path_open_leaf(parent, basename, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
    int error_number = errno;
    free(basename);
    close(parent);
    if (fd < 0) {
        errno = error_number;
        return -1;
    }

    char* value = NULL;
    struct stat status;
    if (fstat(fd, &status) != 0)
        goto fail;
    mode_t mode = status.st_mode & 07777;
    if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() || status.st_nlink != 1 || (mode != 0400 && mode != 0600)) {
        errno = EACCES;
        goto fail;
    }
    const size_t limit = BX_FETCH_BEARER_TOKEN_MAX_BYTES + 2u; /* optional CRLF */
    if (status.st_size < 0 || (uintmax_t)status.st_size > limit) {
        errno = EFBIG;
        goto fail;
    }
    value = malloc(limit + 1u);
    if (!value)
        goto fail;
    size_t length = 0;
    while (length <= limit) {
        ssize_t count = read(fd, value + length, limit + 1u - length);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            goto fail;
        }
        if (count == 0)
            break;
        length += (size_t)count;
    }
    if (length > limit) {
        errno = EFBIG;
        goto fail;
    }
    if (memchr(value, '\0', length)) {
        errno = EINVAL;
        goto fail;
    }
    if (length && value[length - 1u] == '\n') {
        length--;
        if (length && value[length - 1u] == '\r')
            length--;
    }
    value[length] = '\0';
    if (!bx_fetch_http_bearer_token_is_valid(value)) {
        errno = EINVAL;
        goto fail;
    }
    if (close(fd) != 0) {
        free(value);
        return -1;
    }
    free(*token);
    *token = value;
    return 0;

fail:
    error_number = errno;
    free(value);
    close(fd);
    errno = error_number;
    return -1;
}
