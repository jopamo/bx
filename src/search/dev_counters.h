#ifndef BX_SEARCH_DEV_COUNTERS_H
#define BX_SEARCH_DEV_COUNTERS_H

#include <stddef.h>
#include <stdio.h>

void bx_search_dev_counters_begin_from_env(void);
void bx_search_dev_counters_reset(void);
void bx_search_dev_counters_note_bytes_read(size_t count);
void bx_search_dev_counters_note_file_opened(void);
void bx_search_dev_counters_note_candidate_hit(void);
void bx_search_dev_counters_note_matcher_invocation(void);
void bx_search_dev_counters_note_record_materialized(void);
void bx_search_dev_counters_note_output_line_emitted(void);
void bx_search_dev_counters_report(FILE *stream);

#endif
