#include <sys/ioctl.h>
#include <linux/fs.h>

#include "copy_platform.h"

int bx_copy_clone_file(int src_fd, int dest_fd) {
    return ioctl(dest_fd, FICLONE, src_fd);
}
