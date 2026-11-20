/* SPDX-License-Identifier: BSD-2-Clause */
#define _GNU_SOURCE
#include "lib/loop_device.h"

#ifdef __linux__
#include <linux/loop.h>
#include <linux/major.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/sysmacros.h>

#include "lib/fd_ops.h"

int bx_loop_open(const char* path, int flags) {
    int fd = bx_fd_open_cloexec(path, flags | O_NONBLOCK | O_NOCTTY, 0);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) == 0) {
        if (S_ISBLK(st.st_mode) && major(st.st_rdev) == LOOP_MAJOR) {
            char sysfs_path[96];
            int n = snprintf(sysfs_path, sizeof(sysfs_path), "/sys/dev/block/%u:%u",
                             major(st.st_rdev), minor(st.st_rdev));
            if (stat(sysfs_path, &st) == 0) {
                /* A loop ioctl on a partition can reach its parent disk. */
                memcpy(sysfs_path + n, "/partition", sizeof("/partition"));
                if (stat(sysfs_path, &st) == 0)
                    errno = ENODEV;
                else if (errno == ENOENT)
                    return fd;
            }
        } else {
            errno = ENODEV;
        }
    }
    int error = errno;
    bx_fd_cleanup(&fd);
    errno = error;
    return -1;
}

int bx_loop_attach_readonly(int image_fd, const char* name) {
#ifdef LOOP_CONFIGURE
    int control = bx_fd_open_nofollow_cloexec("/dev/loop-control",
                                            O_RDWR | O_NONBLOCK | O_NOCTTY, 0);
    if (control < 0)
        return -1;
    int error = ENODEV, fd = -1;
    struct stat st;
    if (fstat(control, &st) != 0) {
        error = errno;
        goto done;
    }
    /* loop-control is Linux misc device 237, not a loop block node. */
    if (!S_ISCHR(st.st_mode) || major(st.st_rdev) != MISC_MAJOR || minor(st.st_rdev) != 237)
        goto done;

    struct loop_config config = {
        .fd = (uint32_t)image_fd,
        .info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR,
    };
    snprintf((char*)config.info.lo_file_name, sizeof(config.info.lo_file_name), "%s", name);
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        int number = ioctl(control, LOOP_CTL_GET_FREE);
        error = errno;
        if (number < 0)
            break;
        char path[64];
        snprintf(path, sizeof(path), "/dev/loop%d", number);
        /* Keep a reader across mount(2). A writer would block the
         * filesystem's request for a block device without writers. */
        fd = bx_loop_open(path, O_RDONLY | O_NOFOLLOW);
        if (fd >= 0) {
            /* Configure autoclear atomically. No interval with an
             * association that could survive a failed mount or exit. */
            if (ioctl(fd, LOOP_CONFIGURE, &config) == 0)
                break;
            error = errno;
            bx_fd_cleanup(&fd);
        } else
            error = errno;
        if (error != EBUSY && error != ENOENT)
            break;
        if (attempt + 1 < 32)
            (void)nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
    }
done:
    bx_fd_cleanup(&control);
    if (fd < 0)
        errno = (error == ENOTTY || error == ENOSYS) ? ENOTSUP : error;
    return fd;
#else
    (void)image_fd;
    (void)name;
    errno = ENOTSUP;
    return -1;
#endif
}
#endif
