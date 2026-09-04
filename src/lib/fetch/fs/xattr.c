#define _GNU_SOURCE
#include "lib/fetch/xattr.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/xattr.h>
#endif

static int xattr_set_string(int fd, const char* name, const char* value) {
    if (!value || value[0] == '\0')
        return BX_FETCH_XATTR_OK;

#if defined(__linux__)
    int rc = fsetxattr(fd, name, value, strlen(value), 0);
    if (rc == 0) {
        return BX_FETCH_XATTR_OK;
    }

    if (errno == ENOTSUP || errno == EOPNOTSUPP || errno == ENOSYS) {
        return BX_FETCH_XATTR_UNSUPPORTED;
    }

    return BX_FETCH_XATTR_ERROR;
#else
    (void)path;
    (void)name;
    return BX_FETCH_XATTR_UNSUPPORTED;
#endif
}

static int xattr_apply(int fd, const char* url, const char* content_type, const char* etag, const char* last_modified) {
    if (fd < 0 || !url || url[0] == '\0') {
        return BX_FETCH_XATTR_ERROR;
    }

    char* display_url = bx_fetch_url_display_safe(url);
    if (!display_url)
        return BX_FETCH_XATTR_ERROR;

    int rc = BX_FETCH_XATTR_OK;
    const struct {
        const char* name;
        const char* value;
    } attrs[] = {
        {"user.xdg.origin.url", display_url},
        {"user.mime_type", content_type},
        {"user.mira.etag", etag},
        {"user.mira.last_modified", last_modified},
    };
    for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
        int one_rc = xattr_set_string(fd, attrs[i].name, attrs[i].value);
        if (one_rc == BX_FETCH_XATTR_ERROR) {
            free(display_url);
            return BX_FETCH_XATTR_ERROR;
        }
        if (one_rc == BX_FETCH_XATTR_UNSUPPORTED)
            rc = BX_FETCH_XATTR_UNSUPPORTED;
    }
    free(display_url);
    return rc;
}

int bx_fetch_xattr_apply_fd(int fd, const char* url, const char* content_type, const char* etag, const char* last_modified) {
    if (fd < 0)
        return BX_FETCH_XATTR_ERROR;
    return xattr_apply(fd, url, content_type, etag, last_modified);
}
