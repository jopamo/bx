#define _GNU_SOURCE
#include <errno.h>
#include <iconv.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "rg_text.h"

#define BX_RG_DECODE_CHUNK_CAP 8192u

static bool bx_rg_decode_reserve(unsigned char **buf,
                                 size_t *cap,
                                 size_t used,
                                 size_t needed,
                                 size_t limit) {
    size_t new_cap;
    unsigned char *grown;

    if (!buf || !*buf || !cap || needed > limit || used > needed) {
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
    if (!bx_rg_decode_reserve(buf, cap, *len, needed, limit))
        return false;
    if (data_len > 0u)
        memcpy(*buf + *len, data, data_len);
    *len = needed;
    return true;
}

static size_t bx_rg_decode_fread(FILE *input, unsigned char *buf, size_t cap) {
    size_t nread = fread(buf, 1u, cap, input);

    bx_search_dev_counters_note_content_read(nread);
    return nread;
}

static bool bx_rg_decode_stream_copy(FILE *input,
                                     const unsigned char *prefix,
                                     size_t prefix_len,
                                     size_t input_count,
                                     size_t input_limit,
                                     size_t output_limit,
                                     unsigned char **output,
                                     size_t *output_len) {
    unsigned char chunk[BX_RG_DECODE_CHUNK_CAP];
    size_t cap = output_limit < 4096u ? output_limit : 4096u;
    size_t len = 0u;
    unsigned char *buf = malloc(cap + 1u);

    if (!buf) {
        errno = ENOMEM;
        return false;
    }
    if (!bx_rg_decode_append(&buf, &cap, &len, prefix, prefix_len, output_limit))
        goto fail;

    for (;;) {
        size_t nread = bx_rg_decode_fread(input, chunk, sizeof(chunk));

        if (nread == 0u)
            break;
        if (input_count > input_limit || nread > input_limit - input_count) {
            errno = EFBIG;
            goto fail;
        }
        input_count += nread;
        if (!bx_rg_decode_append(&buf, &cap, &len, chunk, nread, output_limit))
            goto fail;
    }
    if (ferror(input)) {
        errno = errno != 0 ? errno : EIO;
        goto fail;
    }
    buf[len] = '\0';
    *output = buf;
    *output_len = len;
    return true;

fail:
    free(buf);
    return false;
}

static bool bx_rg_decode_with_iconv(const char *encoding_name,
                                    const unsigned char *input,
                                    size_t input_len,
                                    size_t output_limit,
                                    unsigned char **output,
                                    size_t *output_len) {
    iconv_t cd;
    size_t cap;
    unsigned char *buf;
    unsigned char *outptr;
    size_t outleft;
    char *inptr;
    size_t inleft;

    if (!encoding_name || !output || !output_len)
        return false;

    cd = iconv_open("UTF-8", encoding_name);
    if (cd == (iconv_t)-1)
        return false;

    cap = output_limit < 4096u ? output_limit : 4096u;
    buf = malloc(cap + 1u);
    if (!buf) {
        iconv_close(cd);
        errno = ENOMEM;
        return false;
    }

    outptr = buf;
    outleft = cap;
    inptr = (char *)(uintptr_t)input;
    inleft = input_len;

    while (inleft > 0u) {
        size_t rc = iconv(cd, &inptr, &inleft, (char **)&outptr, &outleft);
        if (rc != (size_t)-1)
            continue;
        if (errno == E2BIG) {
            size_t used = (size_t)(outptr - buf);
            size_t needed = used < output_limit ? used + 1u : output_limit + 1u;

            if (!bx_rg_decode_reserve(&buf, &cap, used, needed, output_limit)) {
                free(buf);
                iconv_close(cd);
                return false;
            }
            outptr = buf + used;
            outleft = cap - used;
            continue;
        }
        if (errno == EILSEQ || errno == EINVAL) {
            static const unsigned char replacement[] = {0xEFu, 0xBFu, 0xBDu};
            if (outleft < sizeof(replacement)) {
                size_t used = (size_t)(outptr - buf);
                size_t needed;

                if (used > output_limit - (output_limit >= sizeof(replacement)
                                               ? sizeof(replacement) : output_limit)) {
                    free(buf);
                    iconv_close(cd);
                    errno = EFBIG;
                    return false;
                }
                needed = used + sizeof(replacement);
                if (!bx_rg_decode_reserve(&buf, &cap, used, needed, output_limit)) {
                    free(buf);
                    iconv_close(cd);
                    return false;
                }
                outptr = buf + used;
                outleft = cap - used;
            }
            memcpy(outptr, replacement, sizeof(replacement));
            outptr += sizeof(replacement);
            outleft -= sizeof(replacement);
            inptr++;
            inleft--;
            continue;
        }
        free(buf);
        iconv_close(cd);
        return false;
    }

    *output_len = (size_t)(outptr - buf);
    unsigned char *grown = realloc(buf, *output_len + 1u);
    *output = grown ? grown : buf;
    (*output)[*output_len] = '\0';
    iconv_close(cd);
    return true;
}

bool bx_rg_decode_buffer_limited(enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 const unsigned char *input,
                                 size_t input_len,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len) {
    const unsigned char *body = input;
    size_t body_len = input_len;
    const char *effective = encoding_name;

    if (!output || !output_len || output_limit == 0u || output_limit == SIZE_MAX) {
        errno = EINVAL;
        return false;
    }
    *output = NULL;
    *output_len = 0u;

    if (!input) {
        *output = malloc(1u);
        if (!*output)
            return false;
        (*output)[0] = '\0';
        return true;
    }

    if (mode == BX_RG_ENCODING_NONE) {
        if (input_len > output_limit) {
            errno = EFBIG;
            return false;
        }
        *output = malloc(input_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, input, input_len);
        (*output)[input_len] = '\0';
        *output_len = input_len;
        return true;
    }

    if (body_len >= 3u && body[0] == 0xEFu && body[1] == 0xBBu && body[2] == 0xBFu) {
        body += 3u;
        body_len -= 3u;
        if (mode == BX_RG_ENCODING_AUTO || bx_rg_encoding_is_utf8(effective))
            effective = "UTF-8";
    } else if (body_len >= 2u && body[0] == 0xFFu && body[1] == 0xFEu) {
        body += 2u;
        body_len -= 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16LE";
    } else if (body_len >= 2u && body[0] == 0xFEu && body[1] == 0xFFu) {
        body += 2u;
        body_len -= 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16BE";
    } else if (mode == BX_RG_ENCODING_AUTO) {
        if (body_len > output_limit) {
            errno = EFBIG;
            return false;
        }
        *output = malloc(body_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, body, body_len);
        (*output)[body_len] = '\0';
        *output_len = body_len;
        return true;
    }

    if (!effective || bx_rg_encoding_is_utf8(effective)) {
        if (body_len > output_limit) {
            errno = EFBIG;
            return false;
        }
        *output = malloc(body_len + 1u);
        if (!*output)
            return false;
        memcpy(*output, body, body_len);
        (*output)[body_len] = '\0';
        *output_len = body_len;
        return true;
    }

    return bx_rg_decode_with_iconv(effective, body, body_len, output_limit,
                                   output, output_len);
}

bool bx_rg_decode_stream_limited(FILE *input,
                                 enum bx_rg_encoding_mode mode,
                                 const char *encoding_name,
                                 size_t input_limit,
                                 size_t output_limit,
                                 unsigned char **output,
                                 size_t *output_len) {
    unsigned char prefix[3];
    size_t prefix_len;
    size_t body_off = 0u;
    const char *effective = encoding_name;
    iconv_t cd = (iconv_t)-1;
    unsigned char input_chunk[BX_RG_DECODE_CHUNK_CAP + 16u];
    unsigned char output_chunk[BX_RG_DECODE_CHUNK_CAP];
    size_t input_count;
    size_t have;
    size_t cap;
    size_t len = 0u;
    unsigned char *buf = NULL;
    bool eof = false;

    if (!input || !output || !output_len ||
        input_limit == 0u || input_limit == SIZE_MAX ||
        output_limit == 0u || output_limit == SIZE_MAX) {
        errno = EINVAL;
        return false;
    }
    *output = NULL;
    *output_len = 0u;

    prefix_len = bx_rg_decode_fread(input, prefix, sizeof(prefix));
    if (prefix_len == 0u && ferror(input)) {
        errno = errno != 0 ? errno : EIO;
        return false;
    }
    if (prefix_len > input_limit) {
        errno = EFBIG;
        return false;
    }
    input_count = prefix_len;

    if (mode == BX_RG_ENCODING_NONE) {
        return bx_rg_decode_stream_copy(input, prefix, prefix_len, input_count,
                                        input_limit, output_limit, output, output_len);
    }

    if (prefix_len >= 3u &&
        prefix[0] == 0xEFu && prefix[1] == 0xBBu && prefix[2] == 0xBFu) {
        body_off = 3u;
        if (mode == BX_RG_ENCODING_AUTO || bx_rg_encoding_is_utf8(effective))
            effective = "UTF-8";
    } else if (prefix_len >= 2u && prefix[0] == 0xFFu && prefix[1] == 0xFEu) {
        body_off = 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16LE";
    } else if (prefix_len >= 2u && prefix[0] == 0xFEu && prefix[1] == 0xFFu) {
        body_off = 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16BE";
    } else if (mode == BX_RG_ENCODING_AUTO) {
        return bx_rg_decode_stream_copy(input, prefix, prefix_len, input_count,
                                        input_limit, output_limit, output, output_len);
    }

    if (!effective || bx_rg_encoding_is_utf8(effective)) {
        return bx_rg_decode_stream_copy(input, prefix + body_off,
                                        prefix_len - body_off, input_count,
                                        input_limit, output_limit, output, output_len);
    }

    cd = iconv_open("UTF-8", effective);
    if (cd == (iconv_t)-1)
        return false;
    cap = output_limit < 4096u ? output_limit : 4096u;
    buf = malloc(cap + 1u);
    if (!buf) {
        iconv_close(cd);
        errno = ENOMEM;
        return false;
    }
    have = prefix_len - body_off;
    if (have > 0u)
        memcpy(input_chunk, prefix + body_off, have);

    for (;;) {
        if (!eof && have < sizeof(input_chunk)) {
            size_t nread =
                bx_rg_decode_fread(input, input_chunk + have, sizeof(input_chunk) - have);

            if (nread == 0u) {
                if (ferror(input)) {
                    errno = errno != 0 ? errno : EIO;
                    goto fail;
                }
                eof = true;
            } else {
                if (input_count > input_limit || nread > input_limit - input_count) {
                    errno = EFBIG;
                    goto fail;
                }
                input_count += nread;
                have += nread;
            }
        }

        char *inptr = (char *)input_chunk;
        size_t inleft = have;
        bool need_more = false;

        while (inleft > 0u) {
            unsigned char *outptr = output_chunk;
            size_t outleft = sizeof(output_chunk);
            size_t rc = iconv(cd, &inptr, &inleft, (char **)&outptr, &outleft);
            size_t produced = sizeof(output_chunk) - outleft;

            if (!bx_rg_decode_append(&buf, &cap, &len, output_chunk,
                                     produced, output_limit)) {
                goto fail;
            }
            if (rc != (size_t)-1)
                continue;
            if (errno == E2BIG)
                continue;
            if (errno == EILSEQ || (errno == EINVAL && eof)) {
                static const unsigned char replacement[] = {0xEFu, 0xBFu, 0xBDu};

                if (!bx_rg_decode_append(&buf, &cap, &len, replacement,
                                         sizeof(replacement), output_limit)) {
                    goto fail;
                }
                inptr++;
                inleft--;
                continue;
            }
            if (errno == EINVAL) {
                memmove(input_chunk, inptr, inleft);
                have = inleft;
                need_more = true;
                break;
            }
            goto fail;
        }
        if (need_more) {
            if (have == sizeof(input_chunk)) {
                errno = EILSEQ;
                goto fail;
            }
            continue;
        }
        have = 0u;
        if (eof)
            break;
    }

    buf[len] = '\0';
    *output = buf;
    *output_len = len;
    iconv_close(cd);
    return true;

fail:
    free(buf);
    iconv_close(cd);
    return false;
}
