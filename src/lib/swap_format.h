/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_SWAP_FORMAT_H
#define BX_SWAP_FORMAT_H

/* Linux swap v1 header. The magic occupies the end of its first page. */
#define BX_SWAP_V1_MAGIC "SWAPSPACE2"

enum {
    BX_SWAP_VERSION_OFFSET = 1024,
    BX_SWAP_LAST_PAGE_OFFSET = 1028,
    BX_SWAP_BADPAGE_COUNT_OFFSET = 1032,
    BX_SWAP_UUID_OFFSET = 1036,
    BX_SWAP_LABEL_OFFSET = 1052,
    BX_SWAP_LABEL_BYTES = 16,
    BX_SWAP_BADPAGES_OFFSET = 1536,
    BX_SWAP_MIN_PAGE_SIZE = 2048,
    BX_SWAP_MAX_PAGE_SIZE = 65536,
};

#endif
