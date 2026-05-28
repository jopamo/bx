#ifndef BX_SEARCH_SCANNER_H
#define BX_SEARCH_SCANNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

struct bx_literal_matcher;

struct bx_search_candidate {
    size_t chunk_off;
    off_t file_off;
    size_t anchor_len;
};

struct bx_search_record_slice {
    const unsigned char *data;
    size_t len;
    off_t file_off;
    bool has_delim;
    size_t chunk_off;
};

struct bx_search_scanner {
    unsigned char *buf;
    size_t cap;
    size_t len;
    size_t scan_len;
    off_t file_off;
    size_t records_before_buf;
    char delimiter;
    bool track_record_numbers;
    bool eof;
};

void bx_search_scanner_dispose(struct bx_search_scanner *scanner);
bool bx_search_scanner_reserve(struct bx_search_scanner *scanner, size_t needed);
void bx_search_scanner_begin_file(struct bx_search_scanner *scanner,
                                  char delimiter,
                                  bool track_record_numbers);
bool bx_search_scanner_read_chunk(struct bx_search_scanner *scanner, FILE *stream);
bool bx_search_scanner_next_literal_candidate(const struct bx_search_scanner *scanner,
                                              struct bx_literal_matcher *literal,
                                              size_t *cursor,
                                              struct bx_search_candidate *candidate);
bool bx_search_scanner_expand_record(const struct bx_search_scanner *scanner,
                                     const struct bx_search_candidate *candidate,
                                     struct bx_search_record_slice *record);
size_t bx_search_scanner_count_delimiters_range(const struct bx_search_scanner *scanner,
                                                size_t start_off,
                                                size_t end_off);
size_t bx_search_scanner_record_number(const struct bx_search_scanner *scanner,
                                       const struct bx_search_record_slice *record);

#endif
