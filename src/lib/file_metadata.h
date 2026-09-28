#ifndef BX_LIB_FILE_METADATA_H
#define BX_LIB_FILE_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

struct bx_file_xattr {
    char* name;
    unsigned char* value;
    size_t size;
};

/* Zero-initialize before use. Owns all strings and byte arrays; free remains
 * valid after partial failure. Copies own their storage independently. */
struct bx_file_metadata {
    struct bx_file_xattr* xattrs;
    size_t len;
    char* acl_access;
    char* acl_default;
    bool restore_acls;
};

typedef bool (*bx_file_xattr_filter)(const char* name, const void* user);

void bx_file_metadata_free(struct bx_file_metadata* metadata);
bool bx_file_metadata_set(struct bx_file_metadata* metadata, const char* name,
                          const void* value, size_t size);
void bx_file_metadata_copy(struct bx_file_metadata* dest,
                           const struct bx_file_metadata* source);
/* Read into an empty model. fd >= 0 borrows a readable descriptor and never
 * resolves path. Symlinks currently require fd == -1.
 * The filter and context are borrowed only for this call. */
bool bx_file_metadata_read(struct bx_file_metadata* metadata, int fd, const char* path,
                           bool symlink, bool directory, bool acls, bool numeric_ids,
                           bx_file_xattr_filter filter, const void* user);
/* fd identifies an already-open inode. With fd == -1, use no-follow path APIs.
 * Apply after chown/chmod and data writes, which can clear inode metadata.
 * ACLs precede xattrs; security.capability is applied last. */
bool bx_file_metadata_apply(const struct bx_file_metadata* metadata, int fd,
                            const char* path, bool symlink, bool directory,
                            mode_t mode);

#endif
