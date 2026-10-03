#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

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

struct bx_archive_spool {
    int fd;
    uintmax_t len;
};
#define BX_ARCHIVE_SPOOL_INIT {.fd = -1}

/* Open only a closed handle. Failed writes require discarding the private spool. */
bool bx_archive_spool_open(struct bx_archive_spool* spool, struct bx_diag_ctx* diag);
bool bx_archive_spool_close(struct bx_archive_spool* spool, struct bx_diag_ctx* diag);
bool bx_archive_spool_append(struct bx_archive_spool* spool, const void* data, size_t len);
bool bx_archive_spool_append_zeros(struct bx_archive_spool* spool, size_t len);
bool bx_archive_spool_copy(struct bx_archive_spool* spool, int source_fd, off_t length, off_t* copied, struct bx_diag_ctx* diag);
bool bx_archive_spool_write_all(FILE* stream, const struct bx_archive_spool* spool, struct bx_diag_ctx* diag);
bool bx_archive_spool_read_all(struct bx_archive_spool* spool, int source_fd, struct bx_diag_ctx* diag);
bool bx_archive_spool_read_at(const struct bx_archive_spool* spool, uintmax_t offset, void* data, size_t len, struct bx_diag_ctx* diag);
/* Consume a validated span without retaining a descriptor per member. */
bool bx_archive_spool_copy_at(const struct bx_archive_spool* spool, uintmax_t offset, uintmax_t len, int fd, bool sparse, struct bx_diag_ctx* diag);

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
bool bx_archive_buffer_read_all(int fd, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);
bool bx_archive_buffer_write_all(FILE* stream, const struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag);
bool bx_archive_buffer_has_gzip_magic(const struct bx_archive_buffer* buffer);

void bx_archive_name_list_free(struct bx_archive_name_list* list);
bool bx_archive_name_list_append(struct bx_archive_name_list* list, const char* name);
bool bx_archive_name_list_read_fd(int fd, unsigned char separator, struct bx_archive_name_list* list, struct bx_diag_ctx* diag);
bool bx_archive_name_list_read_fd_bounded(int fd, unsigned char separator, struct bx_archive_name_list* list, size_t count_limit, size_t input_byte_limit, struct bx_diag_ctx* diag);
bool bx_archive_name_list_read_path(const char* path,
                                    unsigned char separator,
                                    struct bx_archive_name_list* list,
                                    struct bx_diag_ctx* diag);

bool bx_archive_write_regular_payload(int fd,
                                      const unsigned char* data,
                                      size_t len,
                                      bool sparse,
                                      struct bx_diag_ctx* diag);
/* Copy at most limit bytes at current offsets; sparse output starts at zero in
 * a fresh/truncated destination. Borrow descriptors; the caller verifies source state. */
bool bx_archive_copy_regular_payload(int source_fd, int fd, off_t limit, bool sparse, off_t* copied, struct bx_diag_ctx* diag);

bool bx_archive_output_file_open(struct bx_archive_output_file* out,
                                 const char* archive_path,
                                 struct bx_diag_ctx* diag);
bool bx_archive_output_file_finish(struct bx_archive_output_file* out,
                                   struct bx_diag_ctx* diag);
void bx_archive_output_file_discard(struct bx_archive_output_file* out);

bool bx_archive_path_has_gzip_suffix(const char* path);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_COMMON_H */
