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

struct bx_archive_name_list {
    char** items;
    size_t len;
    size_t cap;
};

struct bx_archive_output_file {
    FILE* stream;
    char* publish_path;
    char* temp_path;
    const char* display_path;
    bool is_stdout;
    bool transactional;
};

void bx_archive_buffer_init(struct bx_archive_buffer* buffer);
void bx_archive_buffer_free(struct bx_archive_buffer* buffer);
bool bx_archive_buffer_append(struct bx_archive_buffer* buffer, const void* data, size_t len);
bool bx_archive_buffer_append_byte(struct bx_archive_buffer* buffer, unsigned char value);
bool bx_archive_buffer_append_zeros(struct bx_archive_buffer* buffer, size_t len);
bool bx_archive_buffer_read_all(FILE* stream, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);
bool bx_archive_buffer_write_all(FILE* stream, const struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);
bool bx_archive_buffer_has_gzip_magic(const struct bx_archive_buffer* buffer);

void bx_archive_name_list_free(struct bx_archive_name_list* list);
bool bx_archive_name_list_append(struct bx_archive_name_list* list, const char* name);
bool bx_archive_name_list_read_stream(FILE* stream,
                                      unsigned char separator,
                                      struct bx_archive_name_list* list,
                                      struct bx_diag_ctx* diag);
bool bx_archive_name_list_read_path(const char* path,
                                    unsigned char separator,
                                    struct bx_archive_name_list* list,
                                    struct bx_diag_ctx* diag);

bool bx_archive_write_regular_payload(int fd,
                                      const unsigned char* data,
                                      size_t len,
                                      bool sparse,
                                      struct bx_diag_ctx* diag);

bool bx_archive_output_file_open(struct bx_archive_output_file* out,
                                 const char* archive_path,
                                 struct bx_diag_ctx* diag);
bool bx_archive_output_file_finish(struct bx_archive_output_file* out,
                                   struct bx_diag_ctx* diag);
void bx_archive_output_file_discard(struct bx_archive_output_file* out);

bool bx_archive_snapshot_input_path(const char* archive_path,
                                    char** snapshot_path_out,
                                    struct bx_diag_ctx* diag);

bool bx_archive_path_has_gzip_suffix(const char* path);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H */
