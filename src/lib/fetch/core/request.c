#define _GNU_SOURCE
#include "lib/fetch/http_header.h"
#include "lib/fetch/request.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct BxFetchRequestBodyFile {
    int fd;
    uint64_t size;
    uint64_t offset;
};

static void request_body_file_free(BxFetchRequestBodyFile* body_file) {
    if (!body_file)
        return;
    if (body_file->fd >= 0) {
        close(body_file->fd);
    }
    free(body_file);
}

static BxFetchRequest* request_new_with_target(const char* method, BxFetchPreparedUrl* target) {
    const char* effective_method = method ? method : "GET";
    if (!bx_fetch_http_method_is_valid(effective_method)) {
        bx_fetch_prepared_url_free(target);
        errno = EINVAL;
        return NULL;
    }

    BxFetchRequest* req = calloc(1, sizeof(BxFetchRequest));
    if (!req) {
        bx_fetch_prepared_url_free(target);
        return NULL;
    }

    req->method = strdup(effective_method);

    req->target = target;
    if (!req->method || !req->target) {
        bx_fetch_request_free(req);
        return NULL;
    }

    return req;
}

BxFetchRequest* bx_fetch_request_new(const char* method, const char* url) {
    return request_new_with_target(method, bx_fetch_url_prepare(url));
}

BxFetchRequest* bx_fetch_request_new_canonical(const char* method, const char* canonical_url) {
    return request_new_with_target(method, bx_fetch_url_prepare_canonical(canonical_url));
}

BxFetchRequest* bx_fetch_request_new_prepared(const char* method, const BxFetchPreparedUrl* target) {
    return request_new_with_target(method, bx_fetch_prepared_url_clone(target));
}

void bx_fetch_request_free(BxFetchRequest* req) {
    if (!req)
        return;

    free(req->method);
    bx_fetch_prepared_url_free(req->target);

    if (req->headers) {
        for (size_t i = 0; i < req->header_count; i++) {
            free(req->headers[i].name);
            free(req->headers[i].value);
        }
        free(req->headers);
    }

    free(req->body);
    request_body_file_free(req->body_file);
    free(req);
}

const BxFetchPreparedUrl* bx_fetch_request_target(const BxFetchRequest* req) {
    return req ? req->target : NULL;
}

const char* bx_fetch_request_url_for_transport(const BxFetchRequest* req) {
    return req ? bx_fetch_prepared_url_transport(req->target) : NULL;
}

const char* bx_fetch_request_url_for_display(const BxFetchRequest* req) {
    return req ? bx_fetch_prepared_url_display(req->target) : NULL;
}

int bx_fetch_request_add_header(BxFetchRequest* req, const char* name, const char* value) {
    if (!req || !name || !value) {
        errno = EINVAL;
        return -1;
    }

    char* normalized_name = NULL;
    char* normalized_value = NULL;
    BxFetchHttpHeaderError header_error = bx_fetch_http_header_normalize_pair(name, value, &normalized_name, &normalized_value);
    if (header_error != BX_FETCH_HTTP_HEADER_OK) {
        errno = header_error == BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY ? ENOMEM : EINVAL;
        return -1;
    }

    if (req->header_count >= req->header_capacity) {
        if (req->header_capacity > SIZE_MAX / 2) {
            free(normalized_name);
            free(normalized_value);
            errno = ENOMEM;
            return -1;
        }
        size_t new_cap = req->header_capacity == 0 ? 8 : req->header_capacity * 2;
        if (new_cap > SIZE_MAX / sizeof(BxFetchHeader)) {
            free(normalized_name);
            free(normalized_value);
            errno = ENOMEM;
            return -1;
        }
        BxFetchHeader* new_headers = realloc(req->headers, new_cap * sizeof(BxFetchHeader));
        if (!new_headers) {
            free(normalized_name);
            free(normalized_value);
            errno = ENOMEM;
            return -1;
        }
        req->headers = new_headers;
        req->header_capacity = new_cap;
    }

    req->headers[req->header_count].name = normalized_name;
    req->headers[req->header_count].value = normalized_value;
    req->header_count++;
    return 0;
}

BxFetchRequestBodyResult bx_fetch_request_set_body(BxFetchRequest* req, const void* data, size_t length) {
    if (!req || (!data && length > 0)) {
        errno = EINVAL;
        return BX_FETCH_REQUEST_BODY_POLICY;
    }

    void* copy = NULL;
    if (length > 0) {
        copy = malloc(length);
        if (!copy) {
            errno = ENOMEM;
            return BX_FETCH_REQUEST_BODY_MEMORY;
        }
        memcpy(copy, data, length);
    }

    free(req->body);
    req->body = copy;
    req->body_len = length;
    request_body_file_free(req->body_file);
    req->body_file = NULL;
    return BX_FETCH_REQUEST_BODY_OK;
}

BxFetchRequestBodyResult bx_fetch_request_set_body_file(BxFetchRequest* req, const char* path) {
    if (!req || !path || path[0] == '\0') {
        errno = EINVAL;
        return BX_FETCH_REQUEST_BODY_POLICY;
    }

    /*
     * O_NONBLOCK makes hostile FIFOs/devices inspectable without allowing the
     * open itself to wait indefinitely. It has no effect on regular-file I/O.
     */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return BX_FETCH_REQUEST_BODY_IO;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int error_number = errno ? errno : EIO;
        close(fd);
        errno = error_number;
        return BX_FETCH_REQUEST_BODY_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return BX_FETCH_REQUEST_BODY_POLICY;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)INT64_MAX) {
        close(fd);
        errno = EFBIG;
        return BX_FETCH_REQUEST_BODY_POLICY;
    }

    BxFetchRequestBodyFile* candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        close(fd);
        errno = ENOMEM;
        return BX_FETCH_REQUEST_BODY_MEMORY;
    }
    candidate->fd = fd;
    candidate->size = (uint64_t)st.st_size;

    free(req->body);
    req->body = NULL;
    req->body_len = 0;
    request_body_file_free(req->body_file);
    req->body_file = candidate;
    return BX_FETCH_REQUEST_BODY_OK;
}

bool bx_fetch_request_has_body_file(const BxFetchRequest* req) {
    return req && req->body_file;
}

uint64_t bx_fetch_request_body_file_size(const BxFetchRequest* req) {
    return bx_fetch_request_has_body_file(req) ? req->body_file->size : 0;
}

int bx_fetch_request_body_file_read(BxFetchRequest* req, void* buffer, size_t capacity, size_t* read_out) {
    if (!req || !req->body_file || (!buffer && capacity > 0) || !read_out) {
        errno = EINVAL;
        return -1;
    }
    *read_out = 0;

    BxFetchRequestBodyFile* body_file = req->body_file;
    uint64_t remaining = body_file->size - body_file->offset;
    if (remaining == 0 || capacity == 0)
        return 0;
    if (remaining < capacity)
        capacity = (size_t)remaining;

    ssize_t nread;
    do {
        nread = pread(body_file->fd, buffer, capacity, (off_t)body_file->offset);
    } while (nread < 0 && errno == EINTR);

    if (nread < 0)
        return -1;
    if (nread == 0) {
        errno = EIO;
        return -1;
    }

    body_file->offset += (uint64_t)nread;
    *read_out = (size_t)nread;
    return 0;
}

int bx_fetch_request_body_file_seek(BxFetchRequest* req, int64_t offset, int origin) {
    if (!req || !req->body_file) {
        errno = EINVAL;
        return -1;
    }

    uint64_t base;
    switch (origin) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = req->body_file->offset;
            break;
        case SEEK_END:
            base = req->body_file->size;
            break;
        default:
            errno = EINVAL;
            return -1;
    }

    uint64_t next;
    if (offset < 0) {
        uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1;
        if (magnitude > base) {
            errno = EINVAL;
            return -1;
        }
        next = base - magnitude;
    }
    else {
        uint64_t positive = (uint64_t)offset;
        if (positive > req->body_file->size - base) {
            errno = EINVAL;
            return -1;
        }
        next = base + positive;
    }

    if (next > req->body_file->size) {
        errno = EINVAL;
        return -1;
    }
    req->body_file->offset = next;
    return 0;
}
