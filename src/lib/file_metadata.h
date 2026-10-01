#ifndef BX_LIB_FILE_METADATA_H
#define BX_LIB_FILE_METADATA_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "lib/mount_identity.h"

/* Borrows fd and name. A leaf target borrows a verified parent, never an
 * object fd; its name is exactly one component. No target owns descriptors. */
struct bx_file_metadata_target {
    int fd;
    const char* name;
    struct stat status;
    struct bx_mount_identity identity;
};

bool bx_file_metadata_target_fd(struct bx_file_metadata_target* target, int fd);
bool bx_file_metadata_target_leaf(struct bx_file_metadata_target* target, int parent_fd, const char* name, const struct stat* expected);
/* Always check identity; unchanged also checks capture-relevant stat state. */
bool bx_file_metadata_target_verify(const struct bx_file_metadata_target* target, bool unchanged);
/* Compare captured state, excluding atime changes caused by reading. */
bool bx_file_metadata_stat_unchanged(const struct stat* before, const struct stat* after);

struct bx_file_xattr {
    char* name;
    unsigned char* value;
    size_t size;
};

enum bx_file_xattr_class {
    BX_FILE_XATTR_ORDINARY,
    BX_FILE_XATTR_ACL_ACCESS,
    BX_FILE_XATTR_ACL_DEFAULT,
    BX_FILE_XATTR_SELINUX,
    BX_FILE_XATTR_CAPABILITY,
};
enum bx_file_acl_mask {
    BX_FILE_ACL_ACCESS = 1u,
    BX_FILE_ACL_DEFAULT = 2u,
    BX_FILE_ACL_ALL = BX_FILE_ACL_ACCESS | BX_FILE_ACL_DEFAULT,
};
enum bx_file_xattr_class bx_file_xattr_classify(const char* name);
/* Validate selected label/capability values without interpreting ordinary
 * xattrs. ACL wire values use the dedicated decoder below. */
bool bx_file_xattr_validate(enum bx_file_xattr_class kind, const void* value, size_t size);

/* Zero-initialize before use. Owns all strings and byte arrays; free remains
 * valid after partial failure. Copies own their storage independently. */
struct bx_file_metadata {
    struct bx_file_xattr* xattrs;
    size_t len;
    char* acl_access;
    char* acl_default;
    char* file_flags;
    unsigned int restore_acls; /* bx_file_acl_mask */
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
    unsigned int flags_set;
    unsigned int flags_clear;
    bool flags_present;
};

enum bx_file_restore_result {
    BX_FILE_RESTORE_OK,
    BX_FILE_RESTORE_STAT_ERROR,
    BX_FILE_RESTORE_METADATA_ERROR,
};

/* Probe required leaf interfaces without referring to an existing object.
 * Call before filesystem mutation. Unsupported flags/interfaces are fatal. */
bool bx_file_restore_leaf_supported(const struct bx_file_restore* restore, mode_t type);
enum bx_file_restore_result bx_file_restore_target(const struct bx_file_restore* restore, const struct bx_file_metadata_target* target);

/* Borrow a real regular-file/directory fd; derive type from the pinned inode.
 * Ownership precedes mode, ACLs/xattrs, timestamps and flags.
 * Reject O_PATH and special-object descriptors without changing metadata. */
enum bx_file_restore_result bx_file_restore_fd(const struct bx_file_restore* restore,
                                               int fd);
/* Apply ownership, mode, ACLs and ordinary xattrs only. The caller must retain
 * final labels/capabilities, timestamps and flags until all inode headers end. */
enum bx_file_restore_result bx_file_restore_fd_base(const struct bx_file_restore* restore, int fd);
/* Prepare a private regular inode for fd-free finalization. Check ownership,
 * ordinary xattrs and timestamps now, but retain extractor ownership/mode
 * 0600. Defer ACLs, labels, capabilities and flags to the final restore. */
enum bx_file_restore_result bx_file_restore_prepare_regular(const struct bx_file_restore* restore, int fd);

typedef bool (*bx_file_xattr_filter)(const char* name, const void* user);

void bx_file_metadata_free(struct bx_file_metadata* metadata);
bool bx_file_metadata_set(struct bx_file_metadata* metadata, const char* name,
                          const void* value, size_t size);
/* Decode a selected Linux ACL xattr into canonical numeric text. Keep the
 * previous text on failure; never add the wire value to ordinary xattrs. */
bool bx_file_metadata_set_acl_xattr(struct bx_file_metadata* metadata, const char* name, const void* value, size_t size);
void bx_file_metadata_remove(struct bx_file_metadata* metadata, const char* name);
/* Replace represented fields with owned copies, retaining unrepresented fields.
 * Empty attribute values and empty ACL/flag text are represented fields. */
void bx_file_metadata_overlay(struct bx_file_metadata* dest, const struct bx_file_metadata* source);
void bx_file_metadata_copy(struct bx_file_metadata* dest,
                           const struct bx_file_metadata* source);
/* Names, values and text, including string terminators but not xattr slots.
 * Return SIZE_MAX if their sum cannot be represented. */
size_t bx_file_metadata_value_bytes(const struct bx_file_metadata* metadata);
/* Read into an empty model through a verified target. Leaf reads use xattrat
 * with no-follow semantics and require mount identity. Capture brackets the
 * batch with identity/stat checks; interface failures are not absence.
 * ACL qualifiers are always numeric. */
bool bx_file_metadata_read_target(struct bx_file_metadata* metadata, const struct bx_file_metadata_target* target, bool acls, bx_file_xattr_filter filter, const void* user);
/* Read Linux inode flags from a borrowed fd. The interface applies only to
 * regular files and directories; other object types report applicable=false. */
bool bx_file_metadata_read_flags(int fd, unsigned int* flags, bool* applicable);
bool bx_file_metadata_apply_flags(int fd, unsigned int set, unsigned int clear);
#endif
