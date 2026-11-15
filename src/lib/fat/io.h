/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_FAT_IO_H
#define BX_FAT_IO_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* create_size > 0 creates a new regular file, never truncates an existing one.
 * Linux block devices are held exclusively until close. Other device types
 * are rejected before data I/O. */
int bx_fat_open(const char *path, bool writable, off_t create_size);
int bx_fat_geometry(int fd, uint64_t *bytes, unsigned *sector_size);
ssize_t bx_fat_read(int fd, void *buffer, size_t size);
ssize_t bx_fat_write(int fd, const void *buffer, size_t size);
ssize_t bx_fat_pread(int fd, void *buffer, size_t size, off_t offset);
ssize_t bx_fat_pwrite(int fd, const void *buffer, size_t size, off_t offset);

#endif
