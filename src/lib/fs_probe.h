/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef BX_FS_PROBE_H
#define BX_FS_PROBE_H

struct bx_fs_signature {
    const char *type;
    unsigned block_size;
    char label[33];
    char fat_boot_label[12];
    char uuid[37];
};

/* Return -1 on I/O error, 0 for no match, 1 for a unique signature, or 2
 * for conflicting signatures. Only a unique match fills in the result. */
int bx_fs_probe(int fd, struct bx_fs_signature *result);

#endif
