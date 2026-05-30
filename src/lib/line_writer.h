#ifndef BX_LIB_LINE_WRITER_H
#define BX_LIB_LINE_WRITER_H

#include <stdbool.h>
#include <stddef.h>

struct bx_line_writer {
    int fd;
    char *buffer;
    size_t capacity;
    size_t length;
    int error;
};

void bx_line_writer_init(struct bx_line_writer *writer, int fd, char *buffer, size_t capacity);
bool bx_line_writer_write(struct bx_line_writer *writer, const void *data, size_t length);
bool bx_line_writer_putc(struct bx_line_writer *writer, char ch);
bool bx_line_writer_puts(struct bx_line_writer *writer, const char *text);
bool bx_line_writer_put_line(struct bx_line_writer *writer, const char *text);
bool bx_line_writer_put_line_len(struct bx_line_writer *writer, const char *text, size_t length);
bool bx_line_writer_flush(struct bx_line_writer *writer);
int bx_line_writer_error(const struct bx_line_writer *writer);

#endif /* BX_LIB_LINE_WRITER_H */
