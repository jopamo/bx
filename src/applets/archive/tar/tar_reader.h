#ifndef BX_APPLETS_ARCHIVE_TAR_TAR_READER_H
#define BX_APPLETS_ARCHIVE_TAR_TAR_READER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "applets/archive/archive_common.h"

#define BX_TAR_BLOCK_SIZE 512u

enum bx_tar_kind {
    BX_TAR_KIND_REG = 0,
    BX_TAR_KIND_DIR,
    BX_TAR_KIND_SYMLINK,
    BX_TAR_KIND_HARDLINK,
    BX_TAR_KIND_FIFO,
};

struct bx_tar_sparse_extent {
    size_t offset;
    size_t size;
};

struct bx_tar_entry {
    char* name;
    char* linkname;
    enum bx_tar_kind kind;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    struct timespec mtime;
    unsigned char* data;
    size_t data_len;
    size_t size;
    bool sparse;
    struct bx_tar_sparse_extent* extents;
    size_t extent_count;
};

struct bx_tar_entry_list {
    struct bx_tar_entry* items;
    size_t len;
    size_t cap;
};

struct bx_tar_reader_stream_options {
    const char* archive_path;
    bool require_gzip;
};

struct bx_tar_stream_visitor_ops {
    void* user;
    bool (*begin_entry)(void* user, const struct bx_tar_entry* entry, struct bx_diag_ctx* diag);
    bool (*visit_payload)(void* user,
                          const struct bx_tar_entry* entry,
                          const unsigned char* data,
                          size_t len,
                          struct bx_diag_ctx* diag);
    bool (*end_entry)(void* user, const struct bx_tar_entry* entry, struct bx_diag_ctx* diag);
    bool stream_sparse_payload;
};

void bx_tar_entry_free(struct bx_tar_entry* entry);
void bx_tar_entry_list_free(struct bx_tar_entry_list* list);
bool bx_tar_entry_list_push(struct bx_tar_entry_list* list, const struct bx_tar_entry* entry);

bool bx_tar_parse_archive_buffer(const struct bx_archive_buffer* archive,
                                 struct bx_tar_entry_list* entries,
                                 struct bx_diag_ctx* diag);

bool bx_tar_collect_archive_stream(const struct bx_tar_reader_stream_options* options,
                                   struct bx_tar_entry_list* entries,
                                   struct bx_diag_ctx* diag);

bool bx_tar_visit_archive_stream(const struct bx_tar_reader_stream_options* options,
                                 const struct bx_tar_stream_visitor_ops* visitor_ops,
                                 struct bx_diag_ctx* diag);

#endif
