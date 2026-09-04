#ifndef BX_FETCH_XATTR_H
#define BX_FETCH_XATTR_H

/* BX_FETCH_HEADER_OWNER: fs */
/* BX_FETCH_HEADER_CONSUMERS: fs, core */

/*
 * Layering contract:
 * - Extended-attribute write semantics are isolated in fs.
 * - Core requests xattr application but does not call platform xattr APIs.
 * - The persisted origin URL is canonicalized without authority userinfo.
 *
 * Ownership and lifetime:
 * - Inputs are borrowed.
 * - Return codes are value semantics: OK / UNSUPPORTED / ERROR.
 */

#define BX_FETCH_XATTR_OK 0
#define BX_FETCH_XATTR_UNSUPPORTED 1
#define BX_FETCH_XATTR_ERROR -1

int bx_fetch_xattr_apply_fd(int fd, const char* url, const char* content_type, const char* etag, const char* last_modified);

#endif  // BX_FETCH_XATTR_H
