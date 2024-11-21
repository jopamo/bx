#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "common/digest_util.h"

static int bx_digest_hex_value(int ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

int bx_digest_fd(void *ctx,
                 size_t ctx_size,
                 bx_digest_init_fn init_fn,
                 bx_digest_update_fn update_fn,
                 bx_digest_final_fn final_fn,
                 int fd,
                 uint8_t *out,
                 size_t out_len) {
    uint8_t buffer[32768];

    (void)out_len;

    memset(ctx, 0, ctx_size);
    init_fn(ctx);

    while (true) {
        ssize_t nread = read(fd, buffer, sizeof(buffer));
        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        update_fn(ctx, buffer, (size_t)nread);
    }

    final_fn(ctx, out);
    return 0;
}

int bx_digest_file(void *ctx,
                   size_t ctx_size,
                   bx_digest_init_fn init_fn,
                   bx_digest_update_fn update_fn,
                   bx_digest_final_fn final_fn,
                   const char *path,
                   uint8_t *out,
                   size_t out_len) {
    if (strcmp(path, "-") == 0) {
        return bx_digest_fd(ctx,
                            ctx_size,
                            init_fn,
                            update_fn,
                            final_fn,
                            STDIN_FILENO,
                            out,
                            out_len);
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    int rc = bx_digest_fd(ctx, ctx_size, init_fn, update_fn, final_fn, fd, out, out_len);
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return rc;
}

void bx_hex_encode_lower(const uint8_t *in, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[i * 2u] = digits[in[i] >> 4u];
        out[i * 2u + 1u] = digits[in[i] & 0x0fu];
    }

    out[len * 2u] = '\0';
}

bool bx_parse_check_line(char *line,
                         size_t digest_len,
                         struct bx_checksum_record *record) {
    size_t line_len = strlen(line);
    size_t hex_len = digest_len * 2u;

    if (digest_len == 0u || digest_len > sizeof(record->digest)) {
        return false;
    }

    if (line_len < (hex_len + 2u)) {
        return false;
    }

    for (size_t i = 0; i < digest_len; i++) {
        int hi = bx_digest_hex_value((unsigned char)line[i * 2u]);
        int lo = bx_digest_hex_value((unsigned char)line[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        record->digest[i] = (uint8_t)((hi << 4) | lo);
    }

    if (line[hex_len] != ' ') {
        return false;
    }

    if (line[hex_len + 1u] == '*') {
        record->binary_mode = true;
    } else if (line[hex_len + 1u] == ' ') {
        record->binary_mode = false;
    } else {
        return false;
    }

    record->digest_len = digest_len;
    record->filename = line + hex_len + 2u;
    return record->filename[0] != '\0';
}
