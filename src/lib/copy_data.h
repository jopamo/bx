#ifndef BX_COMMON_COPY_DATA_H
#define BX_COMMON_COPY_DATA_H

#include <sys/types.h>

enum bx_copy_data_result {
    BX_COPY_DATA_SUCCESS = 0,
    BX_COPY_DATA_READ_ERROR = -1,
    BX_COPY_DATA_WRITE_ERROR = -2,
    BX_COPY_DATA_REFLINK_FAILED = -3,
    BX_COPY_DATA_RANGE_ERROR = -4,
};

enum bx_sparse_mode {
    BX_SPARSE_AUTO = 0,
    BX_SPARSE_ALWAYS,
    BX_SPARSE_NEVER,
};

enum bx_reflink_mode {
    BX_REFLINK_NEVER = 0,
    BX_REFLINK_AUTO,
    BX_REFLINK_ALWAYS,
};

struct bx_copy_data_options {
    enum bx_sparse_mode sparse_mode;
    enum bx_reflink_mode reflink_mode;
};

/* Zero-initialize, use exclusively, then release. Allocate at most 64 KiB,
 * lazily on the first buffered transfer; retain it across files and errors. */
struct bx_copy_buffer {
    char* data;
};

void bx_copy_buffer_release(struct bx_copy_buffer* buffer);

/* Whole-file copy from offset zero into an empty destination. Borrow both
 * descriptors; callers must check identity before truncating a destination.
 * Nonregular inputs and size-zero pseudo-files are read through EOF. */
int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options* opts);
int bx_copy_data_with_buffer(int src_fd, int dest_fd, const struct bx_copy_data_options* opts,
    struct bx_copy_buffer* buffer);

/* Copy length bytes from the current offsets, or to EOF for length == -1.
 * Sparse output requires a seekable regular destination with no existing data
 * ahead of its offset; skipped zeros extend its size at the end of the copy.
 * Borrow both descriptors. An EOF before a bounded length is a read error. */
int bx_copy_data_buffered(int src_fd, int dest_fd, off_t length, enum bx_sparse_mode sparse_mode);
int bx_copy_data_buffered_with_buffer(int src_fd, int dest_fd, off_t length,
    enum bx_sparse_mode sparse_mode, struct bx_copy_buffer* buffer);

#endif /* BX_COMMON_COPY_DATA_H */
