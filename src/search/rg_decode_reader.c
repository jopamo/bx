#define _GNU_SOURCE
#include <errno.h>
#include <iconv.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dev_counters.h"
#include "rg_decode_reader.h"

#define BX_RG_DECODE_READER_INPUT_CAP (8192u + 16u)

struct bx_rg_decode_reader {
    FILE *source;
    bool close_source;
    bool passthrough;
    bool source_eof;
    bool iconv_flushed;
    iconv_t cd;
    size_t input_limit;
    size_t input_count;
    int errnum;

    unsigned char input[BX_RG_DECODE_READER_INPUT_CAP];
    size_t input_off;
    size_t input_len;

    unsigned char replacement[3];
    size_t replacement_off;
    size_t replacement_len;
};

static size_t bx_rg_decode_reader_source_read(struct bx_rg_decode_reader *reader,
                                              unsigned char *buf,
                                              size_t cap) {
    size_t nread;
    int fd;

    if (!reader || !reader->source || !buf || cap == 0u)
        return 0u;
    fd = fileno(reader->source);
    if (fd >= 0) {
        ssize_t rc;

        do {
            rc = read(fd, buf, cap);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0) {
            reader->errnum = errno != 0 ? errno : EIO;
            return 0u;
        }
        nread = (size_t)rc;
    } else {
        nread = fread(buf, 1u, cap, reader->source);
    }
    bx_search_dev_counters_note_content_read(nread);
    if (nread > 0u) {
        if (reader->input_limit > 0u &&
            (reader->input_count > reader->input_limit ||
             nread > reader->input_limit - reader->input_count)) {
            reader->errnum = EFBIG;
            return 0u;
        }
        reader->input_count += nread;
        return nread;
    }
    if (fd < 0 && ferror(reader->source))
        reader->errnum = errno != 0 ? errno : EIO;
    else
        reader->source_eof = true;
    return 0u;
}

static ssize_t bx_rg_decode_reader_result(struct bx_rg_decode_reader *reader,
                                          size_t produced) {
    if (produced > 0u)
        return (ssize_t)produced;
    if (reader && reader->errnum != 0) {
        errno = reader->errnum;
        return -1;
    }
    return 0;
}

static void bx_rg_decode_reader_compact_input(struct bx_rg_decode_reader *reader) {
    size_t available;

    if (!reader || reader->input_off == 0u)
        return;
    available = reader->input_len - reader->input_off;
    if (available > 0u)
        memmove(reader->input, reader->input + reader->input_off, available);
    reader->input_off = 0u;
    reader->input_len = available;
}

static bool bx_rg_decode_reader_refill(struct bx_rg_decode_reader *reader) {
    size_t nread;

    if (!reader || reader->source_eof || reader->errnum != 0)
        return false;
    bx_rg_decode_reader_compact_input(reader);
    if (reader->input_len == sizeof(reader->input)) {
        reader->errnum = EILSEQ;
        return false;
    }
    nread = bx_rg_decode_reader_source_read(
        reader,
        reader->input + reader->input_len,
        sizeof(reader->input) - reader->input_len);
    reader->input_len += nread;
    return nread > 0u;
}

static void bx_rg_decode_reader_queue_replacement(struct bx_rg_decode_reader *reader) {
    static const unsigned char replacement[] = {0xEFu, 0xBFu, 0xBDu};

    if (!reader)
        return;
    memcpy(reader->replacement, replacement, sizeof(replacement));
    reader->replacement_off = 0u;
    reader->replacement_len = sizeof(replacement);
}

static size_t bx_rg_decode_reader_copy_replacement(struct bx_rg_decode_reader *reader,
                                                   unsigned char *output,
                                                   size_t cap) {
    size_t available;
    size_t take;

    if (!reader || !output || cap == 0u ||
        reader->replacement_off >= reader->replacement_len) {
        return 0u;
    }
    available = reader->replacement_len - reader->replacement_off;
    take = available < cap ? available : cap;
    memcpy(output, reader->replacement + reader->replacement_off, take);
    reader->replacement_off += take;
    if (reader->replacement_off == reader->replacement_len) {
        reader->replacement_off = 0u;
        reader->replacement_len = 0u;
    }
    return take;
}

static ssize_t bx_rg_decode_reader_read_passthrough(struct bx_rg_decode_reader *reader,
                                                    unsigned char *output,
                                                    size_t cap) {
    size_t produced = 0u;

    if (reader->input_off < reader->input_len) {
        size_t available = reader->input_len - reader->input_off;
        size_t take = available < cap ? available : cap;

        memcpy(output, reader->input + reader->input_off, take);
        reader->input_off += take;
        produced += take;
    }
    if (produced == cap || reader->source_eof || reader->errnum != 0)
        return bx_rg_decode_reader_result(reader, produced);

    {
        size_t nread =
            bx_rg_decode_reader_source_read(reader, output + produced, cap - produced);
        produced += nread;
    }
    return bx_rg_decode_reader_result(reader, produced);
}

static ssize_t bx_rg_decode_reader_read_iconv(struct bx_rg_decode_reader *reader,
                                              unsigned char *output,
                                              size_t cap) {
    size_t produced = 0u;

    while (produced < cap) {
        size_t replacement = bx_rg_decode_reader_copy_replacement(
            reader, output + produced, cap - produced);
        produced += replacement;
        if (produced == cap)
            break;

        if (reader->input_off == reader->input_len) {
            reader->input_off = 0u;
            reader->input_len = 0u;
            if (!reader->source_eof && reader->errnum == 0)
                (void)bx_rg_decode_reader_refill(reader);
            if (reader->errnum != 0)
                break;
        }

        if (reader->input_off < reader->input_len) {
            char *inptr = (char *)reader->input + reader->input_off;
            size_t inleft = reader->input_len - reader->input_off;
            char *outptr = (char *)output + produced;
            size_t outleft = cap - produced;
            size_t rc = iconv(reader->cd, &inptr, &inleft, &outptr, &outleft);

            reader->input_off = reader->input_len - inleft;
            produced = cap - outleft;
            if (rc != (size_t)-1)
                continue;
            if (errno == E2BIG)
                continue;
            if (errno == EILSEQ || (errno == EINVAL && reader->source_eof)) {
                reader->input_off++;
                bx_rg_decode_reader_queue_replacement(reader);
                continue;
            }
            if (errno == EINVAL) {
                if (!bx_rg_decode_reader_refill(reader) &&
                    reader->errnum == 0 && !reader->source_eof) {
                    reader->errnum = EILSEQ;
                }
                continue;
            }
            reader->errnum = errno != 0 ? errno : EIO;
            break;
        }

        if (!reader->source_eof)
            continue;
        if (!reader->iconv_flushed) {
            char *outptr = (char *)output + produced;
            size_t outleft = cap - produced;
            size_t rc = iconv(reader->cd, NULL, NULL, &outptr, &outleft);

            produced = cap - outleft;
            if (rc == (size_t)-1 && errno == E2BIG)
                continue;
            if (rc == (size_t)-1) {
                reader->errnum = errno != 0 ? errno : EIO;
                break;
            }
            reader->iconv_flushed = true;
            continue;
        }
        break;
    }
    return bx_rg_decode_reader_result(reader, produced);
}

static ssize_t bx_rg_decode_reader_cookie_read(void *cookie,
                                               char *output,
                                               size_t cap) {
    struct bx_rg_decode_reader *reader = cookie;

    if (!reader || !output) {
        errno = EINVAL;
        return -1;
    }
    if (cap == 0u)
        return 0;
    if (reader->errnum != 0) {
        errno = reader->errnum;
        return -1;
    }
    if (reader->passthrough) {
        return bx_rg_decode_reader_read_passthrough(
            reader, (unsigned char *)output, cap);
    }
    return bx_rg_decode_reader_read_iconv(reader, (unsigned char *)output, cap);
}

static int bx_rg_decode_reader_cookie_close(void *cookie) {
    struct bx_rg_decode_reader *reader = cookie;
    int close_status = 0;
    int close_errno = 0;

    if (!reader)
        return 0;
    if (reader->cd != (iconv_t)-1 && iconv_close(reader->cd) != 0) {
        close_status = -1;
        close_errno = errno != 0 ? errno : EIO;
    }
    if (reader->close_source && reader->source && fclose(reader->source) != 0 &&
        close_status == 0) {
        close_status = -1;
        close_errno = errno != 0 ? errno : EIO;
    }
    free(reader);
    if (close_status != 0)
        errno = close_errno;
    return close_status;
}

static FILE *bx_rg_decode_reader_open_fail(struct bx_rg_decode_reader *reader,
                                           int errnum) {
    if (reader) {
        if (reader->cd != (iconv_t)-1)
            iconv_close(reader->cd);
        if (reader->close_source && reader->source)
            fclose(reader->source);
        free(reader);
    }
    errno = errnum != 0 ? errnum : EIO;
    return NULL;
}

FILE *bx_rg_decode_reader_open(FILE *source,
                               bool close_source,
                               enum bx_rg_encoding_mode mode,
                               const char *encoding_name,
                               size_t input_limit) {
    struct bx_rg_decode_reader *reader;
    const char *effective = encoding_name;
    unsigned char prefix[3] = {0};
    size_t prefix_len;
    size_t body_off = 0u;
    cookie_io_functions_t io = {
        .read = bx_rg_decode_reader_cookie_read,
        .write = NULL,
        .seek = NULL,
        .close = bx_rg_decode_reader_cookie_close,
    };
    FILE *decoded;

    if (!source) {
        errno = EINVAL;
        return NULL;
    }
    reader = calloc(1u, sizeof(*reader));
    if (!reader) {
        if (close_source)
            fclose(source);
        errno = ENOMEM;
        return NULL;
    }
    reader->source = source;
    reader->close_source = close_source;
    reader->cd = (iconv_t)-1;
    reader->input_limit = input_limit;

    prefix_len = 0u;
    while (prefix_len < sizeof(prefix) && !reader->source_eof &&
           reader->errnum == 0) {
        prefix_len += bx_rg_decode_reader_source_read(
            reader, prefix + prefix_len, sizeof(prefix) - prefix_len);
    }
    if (reader->errnum != 0)
        return bx_rg_decode_reader_open_fail(reader, reader->errnum);

    if (mode == BX_RG_ENCODING_NONE) {
        reader->passthrough = true;
    } else if (prefix_len >= 3u &&
               prefix[0] == 0xEFu && prefix[1] == 0xBBu && prefix[2] == 0xBFu) {
        body_off = 3u;
        if (mode == BX_RG_ENCODING_AUTO || bx_rg_encoding_is_utf8(effective))
            effective = "UTF-8";
    } else if (prefix_len >= 2u &&
               prefix[0] == 0xFFu && prefix[1] == 0xFEu) {
        body_off = 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16LE";
    } else if (prefix_len >= 2u &&
               prefix[0] == 0xFEu && prefix[1] == 0xFFu) {
        body_off = 2u;
        if (mode == BX_RG_ENCODING_AUTO)
            effective = "UTF-16BE";
    } else if (mode == BX_RG_ENCODING_AUTO) {
        reader->passthrough = true;
    }

    if (mode != BX_RG_ENCODING_NONE && !reader->passthrough &&
        (!effective || bx_rg_encoding_is_utf8(effective))) {
        reader->passthrough = true;
    }
    if (prefix_len > body_off) {
        reader->input_len = prefix_len - body_off;
        memcpy(reader->input, prefix + body_off, reader->input_len);
    }

    if (!reader->passthrough) {
        reader->cd = iconv_open("UTF-8", effective);
        if (reader->cd == (iconv_t)-1) {
            int open_errno = errno != 0 ? errno : EINVAL;
            return bx_rg_decode_reader_open_fail(reader, open_errno);
        }
    }

    decoded = fopencookie(reader, "r", io);
    if (!decoded) {
        int open_errno = errno != 0 ? errno : EIO;
        return bx_rg_decode_reader_open_fail(reader, open_errno);
    }
    return decoded;
}
