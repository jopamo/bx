#define _GNU_SOURCE
#include "lib/mount_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

bool bx_mount_identity_read(int fd, struct bx_mount_identity* identity) {
    struct stat status;
    if (fstat(fd, &status) != 0)
        return false;
    *identity = (struct bx_mount_identity){.device = status.st_dev};
#ifdef STATX_MNT_ID
    struct statx extended;
    if (statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &extended) == 0) {
        if (extended.stx_mask & STATX_MNT_ID) {
            identity->mount = extended.stx_mnt_id;
            identity->has_mount = true;
        }
    }
    else if (errno != ENOSYS && errno != EINVAL && errno != EOPNOTSUPP)
        return false;
#endif
    return true;
}

bool bx_mount_identity_compare(const struct bx_mount_identity* root,
                               const struct bx_mount_identity* entry,
                               bool* same) {
    if (root->has_mount && !entry->has_mount) {
        errno = EOPNOTSUPP;
        return false;
    }
    *same = root->device == entry->device
        && (!root->has_mount || root->mount == entry->mount);
    return true;
}
