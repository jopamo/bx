#include <stdbool.h>
#include <stddef.h>
#include <unistd.h>

#include "copy_data.h"

int bx_copy_data(int src_fd, int dest_fd) {
    char buffer[65536];

    while (true) {
        ssize_t nread = read(src_fd, buffer, sizeof(buffer));
        if (nread == 0) {
            return BX_COPY_DATA_SUCCESS;
        }
        if (nread < 0) {
            return BX_COPY_DATA_READ_ERROR;
        }

        ssize_t written_total = 0;
        while (written_total < nread) {
            ssize_t nwritten = write(dest_fd,
                                     buffer + written_total,
                                     (size_t)(nread - written_total));
            if (nwritten < 0) {
                return BX_COPY_DATA_WRITE_ERROR;
            }
            written_total += nwritten;
        }
    }
}
