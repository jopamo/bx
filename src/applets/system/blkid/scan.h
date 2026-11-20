/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_BLKID_SCAN_H
#define BX_BLKID_SCAN_H

#include <stdbool.h>

/* Visit readable Linux block devices. Each descriptor and path is borrowed
 * until the callback returns; false stops the scan. Return -1 with errno
 * if the device listing itself is unavailable. */
int bx_blkid_scan(bool (*visit)(int, const char *, void *), void *ctx);

#endif
