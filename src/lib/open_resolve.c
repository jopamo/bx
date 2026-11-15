#define _GNU_SOURCE
#include "lib/open_resolve.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>

#ifdef __linux__
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

int
bx_openat_resolve(int dirfd, const char *path, int flags, mode_t mode,
    uint64_t resolve)
{
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
#ifdef __linux__
	struct open_how how = {
		.flags = (unsigned int)flags,
		.mode = mode,
		.resolve = resolve,
	};
	return (int)syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
#else
	(void)dirfd;
	(void)flags;
	(void)mode;
	(void)resolve;
	errno = ENOTSUP;
	return -1;
#endif
}

int
bx_open_beneath(int dirfd, const char *path, int flags)
{
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
#ifdef __linux__
	return bx_openat_resolve(dirfd, path, flags, 0, RESOLVE_BENEATH);
#elif defined(O_RESOLVE_BENEATH)
	return openat(dirfd, path, flags | O_RESOLVE_BENEATH, 0);
#else
	(void)dirfd;
	(void)flags;
	errno = ENOTSUP;
	return -1;
#endif
}
