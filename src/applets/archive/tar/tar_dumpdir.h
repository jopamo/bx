#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_DUMPDIR_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_DUMPDIR_H

#include <stdbool.h>
#include <stddef.h>

#include "bx/diag.h"

struct bx_tar_dumpdir_record {
    char marker;
    char* name;
};

struct bx_tar_dumpdir {
    struct bx_tar_dumpdir_record* records;
    size_t len;
    size_t cap;
};

void bx_tar_dumpdir_free(struct bx_tar_dumpdir* dumpdir);
bool bx_tar_dumpdir_parse(const unsigned char* data, size_t len, struct bx_tar_dumpdir* dumpdir, struct bx_diag_ctx* diag);
const struct bx_tar_dumpdir_record* bx_tar_dumpdir_find(const struct bx_tar_dumpdir* dumpdir, const char* name);

#endif /* BX_APPLETS_ARCHIVE_TAR_TAR_DUMPDIR_H */
