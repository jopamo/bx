#ifndef BX_SEARCH_CHUNK_OVERLAP_H
#define BX_SEARCH_CHUNK_OVERLAP_H

#include "dev_counters.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline size_t bx_search_chunk_overlap_carry_len(size_t scan_len,
                                                       size_t overlap_len) {
    return overlap_len < scan_len ? overlap_len : scan_len;
}

static inline void bx_search_chunk_overlap_preserve_suffix(unsigned char *buf,
                                                           size_t scan_len,
                                                           size_t carry_len) {
    if (!buf || carry_len == 0u || carry_len > scan_len)
        return;
    memmove(buf, buf + scan_len - carry_len, carry_len);
}

static inline size_t bx_search_chunk_overlap_prepend_carry(unsigned char *buf,
                                                           size_t scan_len,
                                                           size_t carry_len) {
    bx_search_chunk_overlap_preserve_suffix(buf, scan_len, carry_len);
    return carry_len <= scan_len ? carry_len : 0u;
}

static inline size_t bx_search_chunk_overlap_scan_start(size_t carry_len,
                                                        size_t overlap_len) {
    return carry_len > overlap_len ? carry_len - overlap_len : 0u;
}

static inline size_t bx_search_chunk_overlap_rescanned_bytes(size_t carry_len,
                                                             size_t overlap_len) {
    return carry_len - bx_search_chunk_overlap_scan_start(carry_len, overlap_len);
}

static inline bool bx_search_chunk_overlap_match_crosses_boundary(size_t match_start,
                                                                  size_t match_end,
                                                                  size_t carry_len) {
    return carry_len > 0u && match_start < carry_len && match_end > carry_len;
}

static inline void bx_search_chunk_overlap_note_cross_chunk_match(size_t match_start,
                                                                  size_t match_end,
                                                                  size_t carry_len) {
    if (bx_search_chunk_overlap_match_crosses_boundary(match_start, match_end, carry_len))
        bx_search_dev_counters_note_literal_cross_chunk_match();
}

static inline bool bx_search_chunk_overlap_has_fresh_bytes(size_t scan_len,
                                                           size_t carry_len) {
    return scan_len > carry_len;
}

#endif
