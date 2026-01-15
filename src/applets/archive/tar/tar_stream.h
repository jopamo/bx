#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_STREAM_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "applets/archive/archive_fs.h"
#include "bx/diag.h"

struct bx_tar_stream_options {
    bool format_ustar;
    bool owner_set;
    bool group_set;
    bool fixed_mtime;
    uid_t owner;
    gid_t group;
    struct timespec mtime;
};

enum bx_tar_stream_kind {
    BX_TAR_STREAM_KIND_REG = 0,
    BX_TAR_STREAM_KIND_DIR,
    BX_TAR_STREAM_KIND_SYMLINK,
    BX_TAR_STREAM_KIND_HARDLINK,
    BX_TAR_STREAM_KIND_FIFO,
};

struct bx_tar_stream_sink {
    void* user;
    bool (*write)(void* user, const void* data, size_t len);
};

bool bx_tar_stream_write_raw_entry(const struct bx_tar_stream_sink* sink,
                                   const char* path,
                                   const char* linkname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   const unsigned char* data,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   struct bx_diag_ctx* diag);

bool bx_tar_stream_write_trailer(const struct bx_tar_stream_sink* sink,
                                 size_t bytes_written,
                                 struct bx_diag_ctx* diag);

bool bx_tar_stream_encode_fs_list(const struct bx_archive_fs_list* files,
                                  const struct bx_tar_stream_options* options,
                                  const struct bx_tar_stream_sink* sink,
                                  struct bx_diag_ctx* diag);

#endif
