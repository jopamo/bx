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

struct bx_tar_sparse_extent;

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
    bool callback_owns_errors;
};

struct bx_tar_stream_live_entry {
    const struct bx_tar_stream_sink* sink;
    size_t data_remaining;
    size_t padding_remaining;
    bool active;
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

bool bx_tar_stream_start_raw_entry(struct bx_tar_stream_live_entry* entry,
                                   const struct bx_tar_stream_sink* sink,
                                   const char* path,
                                   const char* linkname,
                                   enum bx_tar_stream_kind kind,
                                   mode_t mode,
                                   uid_t uid,
                                   gid_t gid,
                                   size_t data_len,
                                   struct timespec mtime,
                                   bool allow_pax,
                                   struct bx_diag_ctx* diag);

bool bx_tar_stream_start_sparse_v1_entry(struct bx_tar_stream_live_entry* entry,
                                         const struct bx_tar_stream_sink* sink,
                                         const char* path,
                                         mode_t mode,
                                         uid_t uid,
                                         gid_t gid,
                                         const struct bx_tar_sparse_extent* extents,
                                         size_t extent_count,
                                         size_t logical_size,
                                         size_t compact_size,
                                         struct timespec mtime,
                                         struct bx_diag_ctx* diag);

bool bx_tar_stream_write_raw_entry_chunk(struct bx_tar_stream_live_entry* entry,
                                         const void* data,
                                         size_t len,
                                         struct bx_diag_ctx* diag);

bool bx_tar_stream_finish_raw_entry(struct bx_tar_stream_live_entry* entry,
                                    struct bx_diag_ctx* diag);

bool bx_tar_stream_write_fs_list_body(const struct bx_archive_fs_list* files,
                                      const struct bx_tar_stream_options* options,
                                      const struct bx_tar_stream_sink* sink,
                                      size_t* bytes_written_io,
                                      struct bx_diag_ctx* diag);

bool bx_tar_stream_encode_fs_list(const struct bx_archive_fs_list* files,
                                  const struct bx_tar_stream_options* options,
                                  const struct bx_tar_stream_sink* sink,
                                  struct bx_diag_ctx* diag);

#endif
