#ifndef BX_LIB_LINE_WRITER_FILE_H
#define BX_LIB_LINE_WRITER_FILE_H

#include <stdbool.h>
#include <stdio.h>

#include "lib/line_writer.h"

struct bx_line_writer_file {
    struct bx_line_writer writer;
    FILE *stream;
    int fd;
    bool close_fd;
    int error;
    char buffer[8192];
};

bool bx_line_writer_file_open(struct bx_line_writer_file *file, int fd, bool close_fd);
FILE *bx_line_writer_file_stream(struct bx_line_writer_file *file);
bool bx_line_writer_file_flush(struct bx_line_writer_file *file);
bool bx_line_writer_file_finish(struct bx_line_writer_file *file);
void bx_line_writer_file_cleanup(struct bx_line_writer_file *file);
int bx_line_writer_file_error(const struct bx_line_writer_file *file);

#endif /* BX_LIB_LINE_WRITER_FILE_H */
