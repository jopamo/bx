#ifndef BX_LIB_OPEN_RESOLVE_H
#define BX_LIB_OPEN_RESOLVE_H

#include <stdint.h>
#include <sys/types.h>

int bx_openat_resolve(int dirfd, const char *path, int flags, mode_t mode,
    uint64_t resolve);
int bx_open_beneath(int dirfd, const char *path, int flags);

#endif
