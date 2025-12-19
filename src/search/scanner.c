#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "literal.h"
#include "scanner.h"

#define BX_SEARCH_SCANNER_CHUNK_CAP 65536u

static bool bx_search_scanner_reserve(struct bx_search_scanner *scanner, size_t needed) {
    if (scanner->cap >= needed)
        return true;

    size_t new_cap = scanner->cap == 0u ? BX_SEARCH_SCANNER_CHUNK_CAP : scanner->cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    unsigned char *tmp = realloc(scanner->buf, new_cap);
    if (!tmp)
        return false;

    scanner->buf = tmp;
    scanner->cap = new_cap;
    return true;
}

static size_t bx_search_scanner_find_last_delimiter(const struct bx_search_scanner *scanner) {
    for (size_t i = scanner->len; i > 0u; --i) {
        if (scanner->buf[i - 1u] == (unsigned char)scanner->delimiter)
            return i;
    }
    return 0u;
}

static size_t bx_search_scanner_count_delimiters(const unsigned char *buf,
                                                 size_t len,
                                                 unsigned char delimiter) {
    size_t count = 0u;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == delimiter)
            count++;
    }
    return count;
}

static size_t bx_search_scanner_record_start(const struct bx_search_scanner *scanner, size_t chunk_off) {
    while (chunk_off > 0u) {
        if (scanner->buf[chunk_off - 1u] == (unsigned char)scanner->delimiter)
            break;
        --chunk_off;
    }
    return chunk_off;
}

static size_t bx_search_scanner_record_end(const struct bx_search_scanner *scanner, size_t chunk_off) {
    while (chunk_off < scanner->scan_len) {
        if (scanner->buf[chunk_off] == (unsigned char)scanner->delimiter)
            return chunk_off + 1u;
        ++chunk_off;
    }
    return scanner->scan_len;
}

void bx_search_scanner_dispose(struct bx_search_scanner *scanner) {
    if (!scanner)
        return;

    free(scanner->buf);
    scanner->buf = NULL;
    scanner->cap = 0u;
    scanner->len = 0u;
    scanner->scan_len = 0u;
    scanner->file_off = 0;
    scanner->records_before_buf = 0u;
    scanner->delimiter = '\n';
    scanner->eof = false;
}

void bx_search_scanner_begin_file(struct bx_search_scanner *scanner, char delimiter) {
    if (!scanner)
        return;

    scanner->len = 0u;
    scanner->scan_len = 0u;
    scanner->file_off = 0;
    scanner->records_before_buf = 0u;
    scanner->delimiter = delimiter;
    scanner->eof = false;
}

bool bx_search_scanner_read_chunk(struct bx_search_scanner *scanner, FILE *stream) {
    if (!scanner || !stream)
        return false;

    if (scanner->scan_len > 0u) {
        size_t consumed = scanner->scan_len;
        size_t consumed_records = bx_search_scanner_count_delimiters(
            scanner->buf, consumed, (unsigned char)scanner->delimiter
        );
        size_t carry_len = scanner->len - consumed;
        if (carry_len > 0u)
            memmove(scanner->buf, scanner->buf + consumed, carry_len);
        scanner->len = carry_len;
        scanner->file_off += (off_t)consumed;
        scanner->records_before_buf += consumed_records;
        scanner->scan_len = 0u;
    }

    for (;;) {
        if (!scanner->eof) {
            if (!bx_search_scanner_reserve(scanner, scanner->len + BX_SEARCH_SCANNER_CHUNK_CAP))
                return false;

            size_t nread = fread(scanner->buf + scanner->len, 1u, scanner->cap - scanner->len, stream);
            scanner->len += nread;
            bx_search_dev_counters_note_bytes_read(nread);
            if (nread == 0u) {
                if (ferror(stream))
                    return false;
                scanner->eof = true;
            }
        }

        size_t complete_len = bx_search_scanner_find_last_delimiter(scanner);
        if (complete_len > 0u) {
            scanner->scan_len = complete_len;
            return true;
        }

        if (scanner->eof) {
            scanner->scan_len = scanner->len;
            return scanner->scan_len > 0u;
        }

        if (!bx_search_scanner_reserve(scanner, scanner->cap == 0u
                                                    ? BX_SEARCH_SCANNER_CHUNK_CAP
                                                    : scanner->cap * 2u)) {
            return false;
        }
    }
}

bool bx_search_scanner_next_literal_candidate(const struct bx_search_scanner *scanner,
                                              struct bx_literal_matcher *literal,
                                              size_t *cursor,
                                              struct bx_search_candidate *candidate) {
    if (!scanner || !literal || !cursor || !candidate)
        return false;
    if (*cursor > scanner->scan_len)
        return false;

    size_t candidate_start = 0u;
    if (!bx_literal_next_candidate(literal, scanner->buf, scanner->scan_len, cursor, &candidate_start))
        return false;

    bx_search_dev_counters_note_candidate_hit();
    candidate->chunk_off = candidate_start;
    candidate->file_off = scanner->file_off + (off_t)candidate_start;
    candidate->anchor_len = bx_literal_len(literal);
    return true;
}

bool bx_search_scanner_expand_record(const struct bx_search_scanner *scanner,
                                     const struct bx_search_candidate *candidate,
                                     struct bx_search_record_slice *record) {
    if (!scanner || !candidate || !record)
        return false;
    if (candidate->chunk_off >= scanner->scan_len)
        return false;

    size_t start = bx_search_scanner_record_start(scanner, candidate->chunk_off);
    size_t end = bx_search_scanner_record_end(scanner, candidate->chunk_off + candidate->anchor_len);
    if (end < start || end > scanner->scan_len)
        return false;

    record->data = scanner->buf + start;
    record->len = end - start;
    record->file_off = scanner->file_off + (off_t)start;
    record->has_delim = (record->len > 0u
                         && record->data[record->len - 1u] == (unsigned char)scanner->delimiter);
    record->chunk_off = start;
    return true;
}

size_t bx_search_scanner_record_number(const struct bx_search_scanner *scanner,
                                       const struct bx_search_record_slice *record) {
    if (!scanner || !record)
        return 1u;

    size_t prior = bx_search_scanner_count_delimiters(scanner->buf,
                                                      record->chunk_off,
                                                      (unsigned char)scanner->delimiter);
    return scanner->records_before_buf + prior + 1u;
}
