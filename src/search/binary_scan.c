#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "binary_scan.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "search_internal.h"

#define BX_SEARCH_BINARY_SCAN_CHUNK_CAP 8192u

static int bx_search_binary_segment_matches(const unsigned char *buf,
                                            size_t len,
                                            struct bx_matcher *m,
                                            struct search_opts *opts) {
    struct bx_match match;
    int match_rc = bx_search_matcher_find_with_opts(m, buf, len, 0u, opts, &match);
    bool matched;

    if (match_rc < 0)
        return -1;
    matched = match_rc == 0;
    if (opts->invert_match)
        matched = !matched;
    return matched ? 1 : 0;
}

static bool bx_search_binary_segment_append(unsigned char **segment,
                                            size_t *segment_len,
                                            size_t *segment_cap,
                                            const unsigned char *data,
                                            size_t len,
                                            size_t limit,
                                            struct bx_record_stream *record_stream) {
    size_t needed;

    if (len == 0u)
        return true;
    if (*segment_len > limit || len > limit - *segment_len) {
        record_stream->errnum = EOVERFLOW;
        return false;
    }
    needed = *segment_len + len;
    if (needed > *segment_cap) {
        size_t new_cap = *segment_cap == 0u ? 256u : *segment_cap;
        unsigned char *tmp;

        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2u) {
                record_stream->errnum = ENOMEM;
                return false;
            }
            new_cap *= 2u;
        }
        tmp = realloc(*segment, new_cap);
        if (!tmp) {
            record_stream->errnum = ENOMEM;
            return false;
        }
        *segment = tmp;
        *segment_cap = new_cap;
    }

    memcpy(*segment + *segment_len, data, len);
    *segment_len = needed;
    return true;
}

int bx_search_binary_scan_remaining(FILE *f,
                                    struct bx_matcher *m,
                                    struct search_opts *opts,
                                    struct bx_record_stream *record_stream,
                                    struct bx_search_stats *stats) {
    unsigned char chunk[BX_SEARCH_BINARY_SCAN_CHUNK_CAP];
    unsigned char *segment = NULL;
    size_t segment_len = 0u;
    size_t segment_cap = 0u;
    size_t segment_limit;
    bool empty_selection_known = false;
    bool empty_selected = false;
    int result = 0;

    if (!f || !m || !opts || !record_stream) {
        errno = EINVAL;
        return -1;
    }
    segment_limit = bx_record_stream_record_limit(record_stream);
    if (segment_limit == 0u)
        segment_limit = bx_record_stream_default_record_limit();

    for (;;) {
        size_t nread = bx_record_stream_read_chunk(f, record_stream, chunk, sizeof(chunk));
        size_t start = 0u;

        if (stats)
            stats->bytes_searched += nread;
        if (nread == 0u)
            break;

        while (start < nread) {
            size_t boundary = start;

            while (boundary < nread &&
                   chunk[boundary] != '\n' &&
                   chunk[boundary] != '\0') {
                boundary++;
            }
            if (!bx_search_binary_segment_append(
                    &segment, &segment_len, &segment_cap,
                    chunk + start, boundary - start, segment_limit,
                    record_stream)) {
                result = -1;
                goto out;
            }
            if (boundary < nread) {
                if (segment_len > 0u) {
                    result = bx_search_binary_segment_matches(
                        segment, segment_len, m, opts);
                    if (result != 0)
                        goto out;
                } else {
                    if (!empty_selection_known) {
                        result = bx_search_binary_segment_matches(
                            chunk, 0u, m, opts);
                        if (result < 0)
                            goto out;
                        empty_selected = result > 0;
                        empty_selection_known = true;
                    }
                    if (empty_selected) {
                        result = 1;
                        goto out;
                    }
                    do {
                        boundary++;
                    } while (boundary < nread &&
                             (chunk[boundary] == '\n' ||
                              chunk[boundary] == '\0'));
                    start = boundary;
                    continue;
                }
                segment_len = 0u;
                boundary++;
            }
            start = boundary;
        }
    }

    if (bx_record_stream_had_error(record_stream)) {
        result = -1;
        goto out;
    }
    if (segment_len > 0u)
        result = bx_search_binary_segment_matches(segment, segment_len, m, opts);

out:
    free(segment);
    return result;
}
