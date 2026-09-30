#ifndef BX_LIB_FILE_METADATA_H
#define BX_LIB_FILE_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

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
    char* file_flags;
    bool restore_acls;
};

struct bx_file_restore {
    struct bx_file_metadata metadata;
    uid_t uid;
    gid_t gid;
    mode_t mode;
    struct timespec mtime;
    bool set_owner;
    bool set_group;
    bool set_mode;
    bool set_mtime;
};

enum bx_file_restore_result {
    BX_FILE_RESTORE_OK,
    BX_FILE_RESTORE_STAT_ERROR,
    BX_FILE_RESTORE_METADATA_ERROR,
};

/* Borrow an inode fd. Ownership precedes mode, ACLs/xattrs and timestamps.
 * O_PATH uses empty-path operations (chmod requires fchmodat2). Symlinks
 * require O_PATH and are never followed. */
enum bx_file_restore_result bx_file_restore_fd(const struct bx_file_restore* restore,
                                               int fd,
                                               bool symlink, bool directory);

typedef bool (*bx_file_xattr_filter)(const char* name, const void* user);

void bx_file_metadata_free(struct bx_file_metadata* metadata);
bool bx_file_metadata_set(struct bx_file_metadata* metadata, const char* name,
                          const void* value, size_t size);
void bx_file_metadata_remove(struct bx_file_metadata* metadata, const char* name);
/* Replace represented fields with owned copies, retaining unrepresented fields.
 * Empty attribute values and empty ACL text are represented fields. */
void bx_file_metadata_overlay(struct bx_file_metadata* dest, const struct bx_file_metadata* source);
void bx_file_metadata_copy(struct bx_file_metadata* dest,
                           const struct bx_file_metadata* source);
/* Read into an empty model from a borrowed FD, never a pathname. O_PATH xattr
 * access requires kernel support; requested xattr read failures are fatal.
 * The filter and context are borrowed only for this call. */
bool bx_file_metadata_read(struct bx_file_metadata* metadata, int fd,
                           bool symlink, bool directory, bool acls, bool numeric_ids,
                           bx_file_xattr_filter filter, const void* user);
/* Read Linux inode flags from a borrowed fd. The interface applies only to
 * regular files and directories; other object types report applicable=false. */
bool bx_file_metadata_read_flags(int fd, unsigned int* flags, bool* applicable);
/* Borrow a verified inode fd. No pathname reconstruction or fallback.
 * O_PATH operations require empty-path kernel support; failure is fatal.
 * Apply after chown/chmod and data writes, which can clear inode metadata.
 * ACLs precede xattrs; security.capability is applied last. */
bool bx_file_metadata_apply(const struct bx_file_metadata* metadata, int fd,
                            bool symlink, bool directory,
                            mode_t mode);

#endif
