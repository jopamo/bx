/* Run with and without -DBX_RSYNC_TEST_WALK to cover both paths on Linux. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "portable_open.h"

int
main(void)
{
	char root[] = "/tmp/bx-rsync-open-XXXXXX";
	char path[sizeof(root) + 32];
	int rootfd, fd, ret = 1;

	if (mkdtemp(root) == NULL)
		return 1;
	snprintf(path, sizeof(path), "%s/sub", root);
	if (mkdir(path, 0700) != 0)
		goto cleanup_root;
	snprintf(path, sizeof(path), "%s/sub/data", root);
	fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
	if (fd < 0)
		goto cleanup_sub;
	close(fd);
	snprintf(path, sizeof(path), "%s/escape", root);
	if (symlink("/etc/passwd", path) != 0)
		goto cleanup_data;

	rootfd = open(root, O_RDONLY | O_DIRECTORY);
	if (rootfd < 0)
		goto cleanup_link;
	fd = bx_rsync_open_beneath(rootfd, "sub/data", O_RDONLY);
	if (fd < 0)
		goto cleanup_fd;
	close(fd);
	fd = bx_rsync_open_beneath(rootfd, "escape", O_RDONLY);
	if (fd >= 0) {
		close(fd);
		goto cleanup_fd;
	}
	fd = bx_rsync_open_beneath(rootfd, "../escape", O_RDONLY);
	if (fd >= 0 || errno != EXDEV) {
		if (fd >= 0)
			close(fd);
		goto cleanup_fd;
	}
	ret = 0;
cleanup_fd:
	close(rootfd);
cleanup_link:
	snprintf(path, sizeof(path), "%s/escape", root);
	unlink(path);
cleanup_data:
	snprintf(path, sizeof(path), "%s/sub/data", root);
	unlink(path);
cleanup_sub:
	snprintf(path, sizeof(path), "%s/sub", root);
	rmdir(path);
cleanup_root:
	rmdir(root);
	if (ret)
		fprintf(stderr, "open_beneath confinement test failed\n");
	return ret;
}
