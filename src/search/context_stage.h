#ifndef BX_SEARCH_CONTEXT_STAGE_H
#define BX_SEARCH_CONTEXT_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct bx_search_staged_record {
    size_t len;
    size_t byte_offset;
    size_t line_number;
    bool selected;
};

struct bx_search_context_interval {
    size_t first;
    size_t last;
};

bool bx_search_staged_record_write(FILE *records,
                                   const struct bx_search_staged_record *record,
                                   const char *text);
int bx_search_staged_record_read(FILE *records,
                                 struct bx_search_staged_record *record,
                                 char *text);
bool bx_search_staged_rewind(FILE *stream);
int bx_search_context_interval_read(
    FILE *intervals,
    struct bx_search_context_interval *interval);
bool bx_search_context_stage_build_intervals(FILE *records,
                                             FILE *intervals,
                                             size_t record_count,
                                             int before_context,
                                             int after_context);

#endif
