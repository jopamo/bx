#include "config.h"
#include "portable_open.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/syscall.h>
#if defined(SYS_openat2) && defined(__has_include) && !defined(BX_RSYNC_TEST_WALK)
#if __has_include(<linux/openat2.h>)
#include <linux/openat2.h>
#define BX_RSYNC_HAVE_OPENAT2 1
#endif
#endif
#endif

/*
 * On older kernels and Unix systems without a native beneath-resolution
 * primitive, walk directory components with O_NOFOLLOW. This rejects all
 * symlink traversal, including links confined to the root. The walk is not
 * atomic against concurrent directory renames; only the native beneath
 * operations provide that guarantee.
 */
static int
open_beneath_walk(int dirfd, const char *path, int flags)
{
	char *copy, *component, *next, *saveptr = NULL;
	int fd, newfd, saved_errno;

	if (*path == '/' || *path == '\0') {
		errno = EINVAL;
		return -1;
	}
	copy = strdup(path);
	if (copy == NULL)
		return -1;
	fd = dirfd == AT_FDCWD ? open(".", O_RDONLY | O_DIRECTORY) : dup(dirfd);
	if (fd < 0) {
		free(copy);
		return -1;
	}
	newfd = -1;
	component = strtok_r(copy, "/", &saveptr);
	while (component != NULL) {
		if (strcmp(component, "..") == 0) {
			errno = EXDEV;
			goto out;
		}
		next = strtok_r(NULL, "/", &saveptr);
		if (strcmp(component, ".") == 0) {
			component = next;
			continue;
		}
		if (next == NULL) {
			newfd = openat(fd, component, flags | O_NOFOLLOW, 0);
			goto out;
		}
		newfd = openat(fd, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		if (newfd < 0)
			goto out;
		close(fd);
		fd = newfd;
		newfd = -1;
		component = next;
	}
	errno = EINVAL;
out:
	saved_errno = errno;
	close(fd);
	free(copy);
	errno = saved_errno;
	return newfd;
}

int
bx_rsync_open_beneath(int dirfd, const char *path, int flags)
{
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
#ifdef BX_RSYNC_HAVE_OPENAT2
	{
		struct open_how how = {
			.flags = (unsigned int)flags,
			.resolve = RESOLVE_BENEATH,
		};
		int fd = syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
		if (fd >= 0 || (errno != ENOSYS && errno != EINVAL && errno != EPERM))
			return fd;
	}
#elif defined(O_RESOLVE_BENEATH)
	return openat(dirfd, path, flags | O_RESOLVE_BENEATH, 0);
#endif
	return open_beneath_walk(dirfd, path, flags);
}
