#ifndef BX_COMMON_COPY_DATA_H
#define BX_COMMON_COPY_DATA_H

enum bx_copy_data_result {
    BX_COPY_DATA_SUCCESS = 0,
    BX_COPY_DATA_READ_ERROR = -1,
    BX_COPY_DATA_WRITE_ERROR = -2,
    BX_COPY_DATA_REFLINK_FAILED = -3,
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

int bx_copy_data(int src_fd, int dest_fd, const struct bx_copy_data_options* opts);

#endif /* BX_COMMON_COPY_DATA_H */
