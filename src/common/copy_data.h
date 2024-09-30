#ifndef BX_COMMON_COPY_DATA_H
#define BX_COMMON_COPY_DATA_H

enum bx_copy_data_result {
    BX_COPY_DATA_SUCCESS = 0,
    BX_COPY_DATA_READ_ERROR = -1,
    BX_COPY_DATA_WRITE_ERROR = -2,
};

int bx_copy_data(int src_fd, int dest_fd);

#endif /* BX_COMMON_COPY_DATA_H */
