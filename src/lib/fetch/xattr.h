#ifndef MIRA_XATTR_H
#define MIRA_XATTR_H

/* MIRA_HEADER_OWNER: fs */
/* MIRA_HEADER_CONSUMERS: fs, core */

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

#define MIRA_XATTR_OK 0
#define MIRA_XATTR_UNSUPPORTED 1
#define MIRA_XATTR_ERROR -1

int mira_xattr_apply_fd(int fd, const char* url, const char* content_type, const char* etag, const char* last_modified);

#endif  // MIRA_XATTR_H
