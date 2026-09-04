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

struct MiraRequestBodyFile {
    int fd;
    uint64_t size;
    uint64_t offset;
};

static void request_body_file_free(MiraRequestBodyFile *body_file) {
    if (!body_file) return;
    if (body_file->fd >= 0) {
        close(body_file->fd);
    }
    free(body_file);
}

static Request *request_new_with_url_state(const char *method, const char *url,
                                           bool url_is_canonical) {
    Request *req = calloc(1, sizeof(Request));
    if (!req) return NULL;

    if (method) {
        req->method = strdup(method);
    } else {
        req->method = strdup("GET");
    }

    if (url) {
        req->url = strdup(url);
        req->url_is_canonical = url_is_canonical;
    }

    if (!req->method || (url && !req->url)) {
        request_free(req);
        return NULL;
    }

    return req;
}

Request *request_new(const char *method, const char *url) {
    return request_new_with_url_state(method, url, false);
}

Request *request_new_canonical(const char *method, const char *canonical_url) {
    return request_new_with_url_state(method, canonical_url, true);
}

void request_free(Request *req) {
    if (!req) return;

    free(req->method);
    free(req->url);
    free(req->display_url);

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

const char *request_url_for_display(const Request *req) {
    if (!req || !req->url) return NULL;
    return req->display_url ? req->display_url : MIRA_URL_DISPLAY_REDACTED;
}

int request_add_header(Request *req, const char *name, const char *value) {
    if (!req || !name || !value) {
        errno = EINVAL;
        return -1;
    }

    char *normalized_name = NULL;
    char *normalized_value = NULL;
    MiraHttpHeaderError header_error =
        mira_http_header_normalize_pair(name, value,
                                        &normalized_name,
                                        &normalized_value);
    if (header_error != MIRA_HTTP_HEADER_OK) {
        errno = header_error == MIRA_HTTP_HEADER_OUT_OF_MEMORY
                    ? ENOMEM
                    : EINVAL;
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
        if (new_cap > SIZE_MAX / sizeof(MiraHeader)) {
            free(normalized_name);
            free(normalized_value);
            errno = ENOMEM;
            return -1;
        }
        MiraHeader *new_headers = realloc(req->headers, new_cap * sizeof(MiraHeader));
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

MiraRequestBodyResult request_set_body_file(Request *req, const char *path) {
    if (!req || !path || path[0] == '\0') {
        errno = EINVAL;
        return MIRA_REQUEST_BODY_POLICY;
    }

    /*
     * O_NONBLOCK makes hostile FIFOs/devices inspectable without allowing the
     * open itself to wait indefinitely. It has no effect on regular-file I/O.
     */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return MIRA_REQUEST_BODY_IO;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int error_number = errno ? errno : EIO;
        close(fd);
        errno = error_number;
        return MIRA_REQUEST_BODY_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return MIRA_REQUEST_BODY_POLICY;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)INT64_MAX) {
        close(fd);
        errno = EFBIG;
        return MIRA_REQUEST_BODY_POLICY;
    }

    MiraRequestBodyFile *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        close(fd);
        errno = ENOMEM;
        return MIRA_REQUEST_BODY_MEMORY;
    }
    candidate->fd = fd;
    candidate->size = (uint64_t)st.st_size;

    free(req->body);
    req->body = NULL;
    req->body_len = 0;
    request_body_file_free(req->body_file);
    req->body_file = candidate;
    return MIRA_REQUEST_BODY_OK;
}

bool request_has_body_file(const Request *req) {
    return req && req->body_file;
}

uint64_t request_body_file_size(const Request *req) {
    return request_has_body_file(req) ? req->body_file->size : 0;
}

int request_body_file_read(Request *req, void *buffer, size_t capacity,
                           size_t *read_out) {
    if (!req || !req->body_file || (!buffer && capacity > 0) || !read_out) {
        errno = EINVAL;
        return -1;
    }
    *read_out = 0;

    MiraRequestBodyFile *body_file = req->body_file;
    uint64_t remaining = body_file->size - body_file->offset;
    if (remaining == 0 || capacity == 0) return 0;
    if (remaining < capacity) capacity = (size_t)remaining;

    ssize_t nread;
    do {
        nread = pread(body_file->fd, buffer, capacity,
                      (off_t)body_file->offset);
    } while (nread < 0 && errno == EINTR);

    if (nread < 0) return -1;
    if (nread == 0) {
        errno = EIO;
        return -1;
    }

    body_file->offset += (uint64_t)nread;
    *read_out = (size_t)nread;
    return 0;
}

int request_body_file_seek(Request *req, int64_t offset, int origin) {
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
    } else {
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
