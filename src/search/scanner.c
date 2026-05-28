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

bool bx_search_scanner_reserve(struct bx_search_scanner *scanner, size_t needed) {
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
    const unsigned char *hit;

    if (!scanner || scanner->len == 0u)
        return 0u;
    hit = memrchr(scanner->buf, (unsigned char)scanner->delimiter, scanner->len);
    if (!hit)
        return 0u;
    return (size_t)(hit - scanner->buf) + 1u;
}

static size_t bx_search_scanner_count_delimiters(const unsigned char *buf,
                                                 size_t len,
                                                 unsigned char delimiter) {
    size_t count = 0u;
    const unsigned char *cursor = buf;
    const unsigned char *end = buf + len;

    while (cursor < end) {
        const unsigned char *hit = memchr(cursor, delimiter, (size_t)(end - cursor));
        if (!hit)
            break;
        count++;
        cursor = hit + 1u;
    }
    bx_search_dev_counters_note_lines_counted(count);
    return count;
}

static size_t bx_search_scanner_record_start(const struct bx_search_scanner *scanner, size_t chunk_off) {
    const unsigned char *hit;

    if (!scanner || chunk_off == 0u)
        return 0u;
    hit = memrchr(scanner->buf, (unsigned char)scanner->delimiter, chunk_off);
    if (!hit)
        return 0u;
    return (size_t)(hit - scanner->buf) + 1u;
}

static size_t bx_search_scanner_record_end(const struct bx_search_scanner *scanner, size_t chunk_off) {
    const unsigned char *hit;

    if (!scanner || chunk_off >= scanner->scan_len)
        return scanner ? scanner->scan_len : 0u;
    hit = memchr(scanner->buf + chunk_off,
                 (unsigned char)scanner->delimiter,
                 scanner->scan_len - chunk_off);
    if (!hit)
        return scanner->scan_len;
    return (size_t)(hit - scanner->buf) + 1u;
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
    scanner->track_record_numbers = false;
    scanner->eof = false;
}

void bx_search_scanner_begin_file(struct bx_search_scanner *scanner,
                                  char delimiter,
                                  bool track_record_numbers) {
    if (!scanner)
        return;

    scanner->len = 0u;
    scanner->scan_len = 0u;
    scanner->file_off = 0;
    scanner->records_before_buf = 0u;
    scanner->delimiter = delimiter;
    scanner->track_record_numbers = track_record_numbers;
    scanner->eof = false;
}

bool bx_search_scanner_read_chunk(struct bx_search_scanner *scanner, FILE *stream) {
    if (!scanner || !stream)
        return false;

    if (scanner->scan_len > 0u) {
        size_t consumed = scanner->scan_len;
        size_t consumed_records = 0u;
        size_t carry_len = scanner->len - consumed;
        if (scanner->track_record_numbers) {
            /*
             * When -n is active we must keep a running count for discarded
             * chunks, even if the first later literal candidate has not been
             * seen yet. This is the eager cross-chunk line-number work that
             * the hot-path audits intentionally pin.
             */
            consumed_records = bx_search_scanner_count_delimiters(
                scanner->buf, consumed, (unsigned char)scanner->delimiter
            );
        }
        if (carry_len > 0u)
            memmove(scanner->buf, scanner->buf + consumed, carry_len);
        scanner->len = carry_len;
        scanner->file_off += (off_t)consumed;
        if (scanner->track_record_numbers)
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
    if (!bx_literal_next_candidate(literal, scanner->buf, scanner->scan_len,
                                   cursor, &candidate_start)) {
        return false;
    }

    bx_search_dev_counters_note_literal_candidate_hit();
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

size_t bx_search_scanner_count_delimiters_range(const struct bx_search_scanner *scanner,
                                                size_t start_off,
                                                size_t end_off) {
    if (!scanner || start_off > end_off || end_off > scanner->scan_len)
        return 0u;
    return bx_search_scanner_count_delimiters(scanner->buf + start_off,
                                              end_off - start_off,
                                              (unsigned char)scanner->delimiter);
}

size_t bx_search_scanner_record_number(const struct bx_search_scanner *scanner,
                                       const struct bx_search_record_slice *record) {
    if (!scanner || !record)
        return 1u;

    size_t prior = bx_search_scanner_count_delimiters_range(scanner, 0u, record->chunk_off);
    return scanner->records_before_buf + prior + 1u;
}
