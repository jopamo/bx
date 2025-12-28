#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"

struct bx_search_dev_counters {
    bool enabled;
    atomic_size_t bytes_read;
    atomic_size_t files_opened;
    atomic_size_t candidate_hits;
    atomic_size_t matcher_invocations;
    atomic_size_t records_materialized;
    atomic_size_t output_lines_emitted;
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
    atomic_store_explicit(&current_dev_counters.bytes_read, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.files_opened, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.candidate_hits, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.matcher_invocations, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.records_materialized, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_lines_emitted, 0u, memory_order_relaxed);
}

void bx_search_dev_counters_note_bytes_read(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.bytes_read, count, memory_order_relaxed);
}

void bx_search_dev_counters_note_file_opened(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.files_opened, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.candidate_hits, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_matcher_invocation(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.matcher_invocations, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_record_materialized(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.records_materialized, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_output_line_emitted(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.output_lines_emitted, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_report(FILE *stream) {
    if (!current_dev_counters.enabled || !stream)
        return;

    fprintf(stream,
            "bx-search-dev-counters: bytes_read=%zu files_opened=%zu candidate_hits=%zu matcher_invocations=%zu records_materialized=%zu output_lines_emitted=%zu\n",
            atomic_load_explicit(&current_dev_counters.bytes_read, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.files_opened, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.candidate_hits, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.matcher_invocations, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.records_materialized, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.output_lines_emitted, memory_order_relaxed));
}
