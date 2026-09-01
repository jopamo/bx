#ifndef BX_SEARCH_CONTEXT_ROLL_H
#define BX_SEARCH_CONTEXT_ROLL_H

#include <stdbool.h>
#include <stddef.h>

struct bx_search_context_record {
    const unsigned char *text;
    size_t len;
    size_t byte_offset;
    size_t line_number;
};

struct bx_search_context_entry {
    size_t byte_index;
    size_t len;
    size_t byte_offset;
    size_t line_number;
};

struct bx_search_context_roll {
    unsigned char *bytes;
    size_t bytes_front;
    size_t bytes_len;
    size_t bytes_cap;

    struct bx_search_context_entry *entries;
    size_t entries_front;
    size_t entries_len;
    size_t entries_cap;
};

void bx_search_context_roll_dispose(struct bx_search_context_roll *roll);
void bx_search_context_roll_clear(struct bx_search_context_roll *roll);
size_t bx_search_context_roll_count(const struct bx_search_context_roll *roll);
bool bx_search_context_roll_push(struct bx_search_context_roll *roll,
                                 const unsigned char *text,
                                 size_t len,
                                 size_t byte_offset,
                                 size_t line_number,
                                 size_t max_records);
bool bx_search_context_roll_get(const struct bx_search_context_roll *roll,
                                size_t index,
                                struct bx_search_context_record *record);

#endif
