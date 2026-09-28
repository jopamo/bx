#ifndef BX_LIB_MOUNT_IDENTITY_H
#define BX_LIB_MOUNT_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct bx_mount_identity {
    dev_t device;
    uint64_t mount;
    bool has_mount;
};

/* Read the identity of the pinned inode. Old kernels fall back to st_dev,
 * which cannot distinguish bind mounts of the same filesystem. */
bool bx_mount_identity_read(int fd, struct bx_mount_identity* identity);
/* Do not silently downgrade a mount-aware traversal partway through. */
bool bx_mount_identity_compare(const struct bx_mount_identity* root,
                               const struct bx_mount_identity* entry,
                               bool* same);

#endif
