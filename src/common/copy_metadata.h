#ifndef BX_COMMON_COPY_METADATA_H
#define BX_COMMON_COPY_METADATA_H

#include <stdbool.h>
#include <sys/stat.h>

enum {
    BX_PRESERVE_MODE = 1u << 0,
    BX_PRESERVE_OWNERSHIP = 1u << 1,
    BX_PRESERVE_TIMESTAMPS = 1u << 2,
    BX_PRESERVE_LINKS = 1u << 3,
    BX_PRESERVE_XATTR = 1u << 4,
};

#define BX_PRESERVE_ALL (BX_PRESERVE_MODE | BX_PRESERVE_OWNERSHIP | BX_PRESERVE_TIMESTAMPS | BX_PRESERVE_LINKS | BX_PRESERVE_XATTR)

bool bx_copy_fd_metadata(int src_fd, int dest_fd, const struct stat* src_stat, unsigned mask);
bool bx_copy_path_metadata(const char* src_path, const char* dest_path, const struct stat* src_stat, unsigned mask, bool no_follow);

#endif /* BX_COMMON_COPY_METADATA_H */
