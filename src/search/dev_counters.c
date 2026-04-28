#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <inttypes.h>
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
    atomic_size_t scanner_plain_prefix_allocs;
    atomic_size_t output_lines_emitted;
    atomic_uint_fast64_t files_seen;
    atomic_uint_fast64_t dirs_seen;
    atomic_uint_fast64_t global_pool_submits;
    atomic_uint_fast64_t global_pool_pops;
    atomic_uint_fast64_t worker_wakeups;
    atomic_uint_fast64_t path_bytes_copied;
    atomic_uint_fast64_t path_copies_before_match;
    atomic_uint_fast64_t batches_built;
    atomic_uint_fast64_t batches_searched;
    atomic_uint_fast64_t empty_batches;
    atomic_uint_fast64_t memstreams_opened;
    atomic_uint_fast64_t output_records_submitted;
    atomic_uint_fast64_t ordered_output_records;
    atomic_uint_fast64_t unordered_output_flushes;
    atomic_uint_fast64_t skipped_output_seqs;
    atomic_uint_fast64_t local_files_searched;
    atomic_uint_fast64_t stolen_files_searched;
    atomic_uint_fast64_t local_dirs_walked;
    atomic_uint_fast64_t stolen_dirs_walked;
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
    atomic_store_explicit(&current_dev_counters.scanner_plain_prefix_allocs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_lines_emitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.files_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.dirs_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_pool_submits, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_pool_pops, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_wakeups, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.path_bytes_copied, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.path_copies_before_match, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.batches_built, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.batches_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.empty_batches, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.memstreams_opened, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_records_submitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.ordered_output_records, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.unordered_output_flushes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.skipped_output_seqs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.local_files_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.stolen_files_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.local_dirs_walked, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.stolen_dirs_walked, 0u, memory_order_relaxed);
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

void bx_search_dev_counters_note_scanner_plain_prefix_alloc(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.scanner_plain_prefix_allocs, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_output_line_emitted(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.output_lines_emitted, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_rg_sched(enum bx_search_rg_sched_counter counter,
                                          uint64_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    switch (counter) {
    case BX_SEARCH_RG_SCHED_FILES_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.files_seen, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_DIRS_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.dirs_seen, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_POOL_SUBMITS:
        atomic_fetch_add_explicit(&current_dev_counters.global_pool_submits, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_POOL_POPS:
        atomic_fetch_add_explicit(&current_dev_counters.global_pool_pops, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_WAKEUPS:
        atomic_fetch_add_explicit(&current_dev_counters.worker_wakeups, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_PATH_BYTES_COPIED:
        atomic_fetch_add_explicit(&current_dev_counters.path_bytes_copied, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_PATH_COPIES_BEFORE_MATCH:
        atomic_fetch_add_explicit(&current_dev_counters.path_copies_before_match, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_BATCHES_BUILT:
        atomic_fetch_add_explicit(&current_dev_counters.batches_built, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_BATCHES_SEARCHED:
        atomic_fetch_add_explicit(&current_dev_counters.batches_searched, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_EMPTY_BATCHES:
        atomic_fetch_add_explicit(&current_dev_counters.empty_batches, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED:
        atomic_fetch_add_explicit(&current_dev_counters.memstreams_opened, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_RECORDS_SUBMITTED:
        atomic_fetch_add_explicit(&current_dev_counters.output_records_submitted, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_ORDERED_OUTPUT_RECORDS:
        atomic_fetch_add_explicit(&current_dev_counters.ordered_output_records, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_UNORDERED_OUTPUT_FLUSHES:
        atomic_fetch_add_explicit(&current_dev_counters.unordered_output_flushes, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SKIPPED_OUTPUT_SEQS:
        atomic_fetch_add_explicit(&current_dev_counters.skipped_output_seqs, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_LOCAL_FILES_SEARCHED:
        atomic_fetch_add_explicit(&current_dev_counters.local_files_searched, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_STOLEN_FILES_SEARCHED:
        atomic_fetch_add_explicit(&current_dev_counters.stolen_files_searched, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_LOCAL_DIRS_WALKED:
        atomic_fetch_add_explicit(&current_dev_counters.local_dirs_walked, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_STOLEN_DIRS_WALKED:
        atomic_fetch_add_explicit(&current_dev_counters.stolen_dirs_walked, count, memory_order_relaxed);
        return;
    }
}

void bx_search_dev_counters_report(FILE *stream) {
    if (!current_dev_counters.enabled || !stream)
        return;

    fprintf(stream,
            "bx-search-dev-counters: bytes_read=%zu files_opened=%zu candidate_hits=%zu matcher_invocations=%zu records_materialized=%zu scanner_plain_prefix_allocs=%zu output_lines_emitted=%zu "
            "files_seen=%" PRIuMAX " dirs_seen=%" PRIuMAX " global_pool_submits=%" PRIuMAX " global_pool_pops=%" PRIuMAX " worker_wakeups=%" PRIuMAX " "
            "path_bytes_copied=%" PRIuMAX " path_copies_before_match=%" PRIuMAX " batches_built=%" PRIuMAX " batches_searched=%" PRIuMAX " empty_batches=%" PRIuMAX " "
            "memstreams_opened=%" PRIuMAX " output_records_submitted=%" PRIuMAX " ordered_output_records=%" PRIuMAX " unordered_output_flushes=%" PRIuMAX " "
            "skipped_output_seqs=%" PRIuMAX " local_files_searched=%" PRIuMAX " stolen_files_searched=%" PRIuMAX " local_dirs_walked=%" PRIuMAX " stolen_dirs_walked=%" PRIuMAX "\n",
            atomic_load_explicit(&current_dev_counters.bytes_read, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.files_opened, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.candidate_hits, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.matcher_invocations, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.records_materialized, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.scanner_plain_prefix_allocs,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.output_lines_emitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.files_seen, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.dirs_seen, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_submits, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_pops, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_wakeups, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.path_bytes_copied, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.path_copies_before_match, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.batches_built, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.batches_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.empty_batches, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.memstreams_opened, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.output_records_submitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.ordered_output_records, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.unordered_output_flushes, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.skipped_output_seqs, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_files_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.stolen_files_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_dirs_walked, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.stolen_dirs_walked, memory_order_relaxed));
}
