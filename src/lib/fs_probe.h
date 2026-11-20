/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_FS_PROBE_H
#define BX_FS_PROBE_H

enum bx_probe_usage {
    BX_PROBE_FILESYSTEM = 1,
    BX_PROBE_OTHER = 2,
    BX_PROBE_CRYPTO = 4,
};

struct bx_fs_signature {
    const char *type;
    const char *version;
    enum bx_probe_usage usage;
    unsigned block_size;
    char label[49];
    char fat_boot_label[12];
    char uuid[37];
};

/* Probe only signatures in the usage mask. Return -1 on I/O error,
 * 0 for no match, 1 for a unique signature, or 2 for conflicting
 * signatures. Only a unique match fills in the result. */
int bx_fs_probe(int fd, unsigned usages, struct bx_fs_signature *result);

#endif
