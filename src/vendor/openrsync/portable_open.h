#ifndef BX_RSYNC_PORTABLE_OPEN_H
#define BX_RSYNC_PORTABLE_OPEN_H

/* Confined open on supporting kernels; symlink-free walk otherwise. */
int bx_rsync_open_beneath(int dirfd, const char *path, int flags);

#endif
