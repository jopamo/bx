#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"

struct bx_search_dev_counters {
    bool enabled;
    size_t bytes_read;
    size_t files_opened;
    size_t candidate_hits;
    size_t matcher_invocations;
    size_t records_materialized;
    size_t output_lines_emitted;
};

static struct bx_search_dev_counters current_dev_counters = {0};

void bx_search_dev_counters_begin_from_env(void) {
    bx_search_dev_counters_reset();

    const char *value = getenv("BX_SEARCH_DEV_COUNTERS");
    if (!value || !*value || strcmp(value, "0") == 0)
        return;

    current_dev_counters.enabled = true;
}

void bx_search_dev_counters_reset(void) {
    current_dev_counters.enabled = false;
    current_dev_counters.bytes_read = 0u;
    current_dev_counters.files_opened = 0u;
    current_dev_counters.candidate_hits = 0u;
    current_dev_counters.matcher_invocations = 0u;
    current_dev_counters.records_materialized = 0u;
    current_dev_counters.output_lines_emitted = 0u;
}

void bx_search_dev_counters_note_bytes_read(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    current_dev_counters.bytes_read += count;
}

void bx_search_dev_counters_note_file_opened(void) {
    if (!current_dev_counters.enabled)
        return;

    current_dev_counters.files_opened++;
}

void bx_search_dev_counters_note_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    current_dev_counters.candidate_hits++;
}

void bx_search_dev_counters_note_matcher_invocation(void) {
    if (!current_dev_counters.enabled)
        return;

    current_dev_counters.matcher_invocations++;
}

void bx_search_dev_counters_note_record_materialized(void) {
    if (!current_dev_counters.enabled)
        return;

    current_dev_counters.records_materialized++;
}

void bx_search_dev_counters_note_output_line_emitted(void) {
    if (!current_dev_counters.enabled)
        return;

    current_dev_counters.output_lines_emitted++;
}

void bx_search_dev_counters_report(FILE *stream) {
    if (!current_dev_counters.enabled || !stream)
        return;

    fprintf(stream,
            "bx-search-dev-counters: bytes_read=%zu files_opened=%zu candidate_hits=%zu matcher_invocations=%zu records_materialized=%zu output_lines_emitted=%zu\n",
            current_dev_counters.bytes_read,
            current_dev_counters.files_opened,
            current_dev_counters.candidate_hits,
            current_dev_counters.matcher_invocations,
            current_dev_counters.records_materialized,
            current_dev_counters.output_lines_emitted);
}
