#ifndef BX_COMMON_STATX_COMPAT_H
#define BX_COMMON_STATX_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#ifndef AT_STATX_DONT_SYNC
#define _ASM_GENERIC_FCNTL_H
#include <linux/fcntl.h>
#endif

#ifndef STATX_TYPE
#include <linux/stat.h>
#endif

#ifndef STATX_TYPE
#define STATX_TYPE 0
#define STATX_MODE 0
#define STATX_NLINK 0
#define STATX_UID 0
#define STATX_GID 0
#define STATX_ATIME 0
#define STATX_MTIME 0
#define STATX_CTIME 0
#define STATX_INO 0
#define STATX_SIZE 0
#define STATX_BLOCKS 0
#define STATX_BASIC_STATS 0
#define STATX_BTIME 0
#define STATX_ALL 0
#endif

#ifndef STATX_MNT_ID
#define STATX_MNT_ID 0x1000U
static inline uint64_t bx_statx_mount_id(const struct statx* result) {
    /* Older libc declarations leave the Linux mount-ID slot as padding. */
    _Static_assert(offsetof(struct statx, stx_dev_minor) == 0x8c && sizeof(struct statx) >= 0x98,
                   "statx mount ID requires the Linux ABI layout");
    uint64_t mount;
    memcpy(&mount, (const unsigned char*)result + 0x90, sizeof(mount));
    return mount;
}
#else
static inline uint64_t bx_statx_mount_id(const struct statx* result) {
    return result->stx_mnt_id;
}
#endif

extern int bx_statx_flags;

int bx_statx_stat(const char* pathname, unsigned int mask, struct stat* st);
int bx_statx_fstat(int fd, unsigned int mask, struct stat* st);
int bx_statx_lstat(const char* pathname, unsigned int mask, struct stat* st);
int bx_statx_get_btime(const char* pathname, struct timespec* btime, bool* available);

#endif /* BX_COMMON_STATX_COMPAT_H */
