#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rg_decode_reader.h"
#include "rg_text.h"

#define BX_RG_DECODE_MATERIALIZE_CHUNK_CAP 8192u

static bool bx_rg_decode_reserve(unsigned char **buf,
                                 size_t *cap,
                                 size_t needed,
                                 size_t limit) {
    size_t new_cap;
    unsigned char *grown;

    if (!buf || !*buf || !cap || needed > limit) {
        errno = needed > limit ? EFBIG : EINVAL;
        return false;
    }
    if (*cap >= needed)
        return true;

    new_cap = *cap;
    while (new_cap < needed) {
        if (new_cap >= limit) {
            errno = EFBIG;
            return false;
        }
        new_cap = new_cap > limit / 2u ? limit : new_cap * 2u;
    }
    grown = realloc(*buf, new_cap + 1u);
    if (!grown) {
        errno = ENOMEM;
        return false;
    }
    *buf = grown;
    *cap = new_cap;
    return true;
}

static bool bx_rg_decode_append(unsigned char **buf,
                                size_t *cap,
                                size_t *len,
                                const unsigned char *data,
                                size_t data_len,
                                size_t limit) {
    size_t needed;

    if (!buf || !cap || !len || *len > limit || data_len > limit - *len) {
        errno = EFBIG;
        return false;
    }
    needed = *len + data_len;
    if (!bx_rg_decode_reserve(buf, cap, needed, limit))
        return false;
    if (data_len > 0u)
        memcpy(*buf + *len, data, data_len);
    *len = needed;
    return true;
}

bool bx_rg_decode_stream_limited(FILE *input,
                                 enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 size_t input_limit,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len) {
    unsigned char chunk[BX_RG_DECODE_MATERIALIZE_CHUNK_CAP];
    size_t cap;
    size_t len = 0u;
    unsigned char *buf;
    FILE *decoded;
    int decode_errno = 0;

    if (!input || !output || !output_len ||
        input_limit == 0u || input_limit == SIZE_MAX ||
        output_limit == 0u || output_limit == SIZE_MAX) {
        errno = EINVAL;
        return false;
    }
    *output = NULL;
    *output_len = 0u;

    decoded = bx_rg_decode_reader_open(input, false, mode, encoding_name, input_limit);
    if (!decoded)
        return false;

    cap = output_limit < 4096u ? output_limit : 4096u;
    buf = malloc(cap + 1u);
    if (!buf) {
        fclose(decoded);
        errno = ENOMEM;
        return false;
    }

    for (;;) {
        size_t nread = fread(chunk, 1u, sizeof(chunk), decoded);

        if (nread > 0u &&
            !bx_rg_decode_append(&buf, &cap, &len, chunk, nread, output_limit)) {
            decode_errno = errno != 0 ? errno : EIO;
            break;
        }
        if (nread == 0u) {
            if (ferror(decoded))
                decode_errno = errno != 0 ? errno : EIO;
            break;
        }
    }
    if (fclose(decoded) != 0 && decode_errno == 0)
        decode_errno = errno != 0 ? errno : EIO;
    if (decode_errno != 0) {
        free(buf);
        errno = decode_errno;
        return false;
    }

    buf[len] = '\0';
    *output = buf;
    *output_len = len;
    return true;
}

bool bx_rg_decode_buffer_limited(enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 const unsigned char *input,
                                 size_t input_len,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len) {
    FILE *source;
    bool decoded;
    int decode_errno;

    if (!output || !output_len || output_limit == 0u || output_limit == SIZE_MAX ||
        (!input && input_len > 0u)) {
        errno = EINVAL;
        return false;
    }
    *output = NULL;
    *output_len = 0u;
    if (input_len == 0u) {
        *output = malloc(1u);
        if (!*output) {
            errno = ENOMEM;
            return false;
        }
        (*output)[0] = '\0';
        return true;
    }

    source = fmemopen((void *)(uintptr_t)input, input_len, "r");
    if (!source)
        return false;
    decoded = bx_rg_decode_stream_limited(
        source, mode, encoding_name, input_len, output_limit, output, output_len);
    decode_errno = errno;
    if (fclose(source) != 0 && decoded) {
        free(*output);
        *output = NULL;
        *output_len = 0u;
        decoded = false;
        decode_errno = errno != 0 ? errno : EIO;
    }
    errno = decode_errno;
    return decoded;
}
