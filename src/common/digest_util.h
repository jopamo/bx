#ifndef BX_COMMON_DIGEST_UTIL_H
#define BX_COMMON_DIGEST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (*bx_digest_init_fn)(void *ctx);
typedef void (*bx_digest_update_fn)(void *ctx, const void *data, size_t len);
typedef void (*bx_digest_final_fn)(void *ctx, uint8_t *out);

int bx_digest_fd(void *ctx,
                 size_t ctx_size,
                 bx_digest_init_fn init_fn,
                 bx_digest_update_fn update_fn,
                 bx_digest_final_fn final_fn,
                 int fd,
                 uint8_t *out,
                 size_t out_len);

int bx_digest_file(void *ctx,
                   size_t ctx_size,
                   bx_digest_init_fn init_fn,
                   bx_digest_update_fn update_fn,
                   bx_digest_final_fn final_fn,
                   const char *path,
                   uint8_t *out,
                   size_t out_len);

void bx_hex_encode_lower(const uint8_t *in, size_t len, char *out);

struct bx_checksum_record {
    uint8_t digest[64];
    size_t digest_len;
    bool binary_mode;
    const char *filename;
};

bool bx_parse_check_line(char *line,
                         size_t digest_len,
                         struct bx_checksum_record *record);

#endif /* BX_COMMON_DIGEST_UTIL_H */
