#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "bx/diag.h"

struct bx_archive_buffer {
    unsigned char* data;
    size_t len;
    size_t cap;
};

void bx_archive_buffer_init(struct bx_archive_buffer* buffer);
void bx_archive_buffer_free(struct bx_archive_buffer* buffer);
bool bx_archive_buffer_append(struct bx_archive_buffer* buffer, const void* data, size_t len);
bool bx_archive_buffer_append_byte(struct bx_archive_buffer* buffer, unsigned char value);
bool bx_archive_buffer_append_zeros(struct bx_archive_buffer* buffer, size_t len);
bool bx_archive_buffer_read_all(FILE* stream, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);
bool bx_archive_buffer_write_all(FILE* stream, const struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);

bool bx_archive_run_gzip_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag);

bool bx_archive_write_regular_payload(int fd,
                                      const unsigned char* data,
                                      size_t len,
                                      bool sparse,
                                      struct bx_diag_ctx* diag);

bool bx_archive_path_has_gzip_suffix(const char* path);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H */
