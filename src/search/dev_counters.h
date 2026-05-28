#ifndef BX_SEARCH_DEV_COUNTERS_H
#define BX_SEARCH_DEV_COUNTERS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

enum bx_search_rg_sched_counter {
    BX_SEARCH_RG_SCHED_FILES_SEEN = 0,
    BX_SEARCH_RG_SCHED_DIRS_SEEN,
    BX_SEARCH_RG_SCHED_GLOBAL_POOL_SUBMITS,
    BX_SEARCH_RG_SCHED_GLOBAL_POOL_POPS,
    BX_SEARCH_RG_SCHED_WORKER_WAKEUPS,
    BX_SEARCH_RG_SCHED_PATH_BYTES_COPIED,
    BX_SEARCH_RG_SCHED_PATH_COPIES_BEFORE_MATCH,
    BX_SEARCH_RG_SCHED_BATCHES_BUILT,
    BX_SEARCH_RG_SCHED_BATCHES_SEARCHED,
    BX_SEARCH_RG_SCHED_EMPTY_BATCHES,
    BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED,
    BX_SEARCH_RG_SCHED_OUTPUT_RECORDS_SUBMITTED,
    BX_SEARCH_RG_SCHED_ORDERED_OUTPUT_RECORDS,
    BX_SEARCH_RG_SCHED_UNORDERED_OUTPUT_FLUSHES,
    BX_SEARCH_RG_SCHED_SKIPPED_OUTPUT_SEQS,
    BX_SEARCH_RG_SCHED_LOCAL_FILES_SEARCHED,
    BX_SEARCH_RG_SCHED_STOLEN_FILES_SEARCHED,
    BX_SEARCH_RG_SCHED_LOCAL_DIRS_WALKED,
    BX_SEARCH_RG_SCHED_STOLEN_DIRS_WALKED,
};

void bx_search_dev_counters_begin_from_env(void);
void bx_search_dev_counters_reset(void);
void bx_search_dev_counters_note_bytes_read(size_t count);
void bx_search_dev_counters_note_file_opened(void);
void bx_search_dev_counters_note_candidate_hit(void);
void bx_search_dev_counters_note_literal_candidate_hit(void);
void bx_search_dev_counters_note_literal_confirm_call(void);
void bx_search_dev_counters_note_literal_plan_compile(void);
void bx_search_dev_counters_note_literal_algo_empty_call(void);
void bx_search_dev_counters_note_literal_algo_byte_call(void);
void bx_search_dev_counters_note_literal_algo_pair_call(void);
void bx_search_dev_counters_note_literal_algo_short_call(void);
void bx_search_dev_counters_note_literal_algo_rare_pair_call(void);
void bx_search_dev_counters_note_literal_algo_long_call(void);
void bx_search_dev_counters_note_literal_algo_scalar_call(void);
void bx_search_dev_counters_note_literal_algo_x86_avx2_call(void);
void bx_search_dev_counters_note_literal_algo_arm64_neon_call(void);
void bx_search_dev_counters_note_literal_algo_arm64_sve_call(void);
void bx_search_dev_counters_note_literal_algo_memmem_call(void);
void bx_search_dev_counters_note_literal_bytes_scanned(size_t count);
void bx_search_dev_counters_note_literal_algo_sse2_call(void);
void bx_search_dev_counters_note_matcher_invocation(void);
void bx_search_dev_counters_note_record_materialized(void);
void bx_search_dev_counters_note_scanner_entry(void);
void bx_search_dev_counters_note_scanner_plain_prefix_alloc(void);
void bx_search_dev_counters_note_output_line_emitted(void);
void bx_search_dev_counters_note_rg_sched(enum bx_search_rg_sched_counter counter,
                                          uint64_t count);
void bx_search_dev_counters_report(FILE *stream);

#endif
