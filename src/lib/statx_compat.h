#ifndef BX_COMMON_STATX_COMPAT_H
#define BX_COMMON_STATX_COMPAT_H

#include <stdbool.h>
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

extern int bx_statx_flags;

int bx_statx_stat(const char* pathname, unsigned int mask, struct stat* st);
int bx_statx_fstat(int fd, unsigned int mask, struct stat* st);
int bx_statx_lstat(const char* pathname, unsigned int mask, struct stat* st);
int bx_statx_get_btime(const char* pathname, struct timespec* btime, bool* available);

#endif /* BX_COMMON_STATX_COMPAT_H */
