#ifndef BX_SEARCH_DEV_COUNTERS_H
#define BX_SEARCH_DEV_COUNTERS_H

#include <stdbool.h>
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
    BX_SEARCH_RG_SCHED_QUEUED_OUTPUT_BATCHES,
    BX_SEARCH_RG_SCHED_EMPTY_OUTPUT_BATCHES,
    BX_SEARCH_RG_SCHED_WORKER_SUBTREES_DONATED,
};

enum bx_search_walk_counter {
    BX_SEARCH_WALK_DIRENTS_SEEN = 0,
    BX_SEARCH_WALK_DIRS_SEEN,
    BX_SEARCH_WALK_FILES_SEEN,
    BX_SEARCH_WALK_SYMLINKS_SEEN,
    BX_SEARCH_WALK_UNKNOWN_DTYPE_SEEN,
    BX_SEARCH_WALK_LSTAT_CALLS,
    BX_SEARCH_WALK_FSTATAT_CALLS,
    BX_SEARCH_WALK_STAT_REASON_UNKNOWN_DTYPE,
    BX_SEARCH_WALK_STAT_REASON_SYMLINK_POLICY,
    BX_SEARCH_WALK_STAT_REASON_TRAVERSAL_POLICY,
    BX_SEARCH_WALK_STAT_REASON_METADATA_FILTER,
    BX_SEARCH_WALK_STAT_REASON_METADATA_OUTPUT,
    BX_SEARCH_WALK_OPENAT_CALLS,
    BX_SEARCH_WALK_PATH_JOIN_CALLS,
    BX_SEARCH_WALK_PATH_ALLOCS,
    BX_SEARCH_WALK_PATH_COPIES_BEFORE_MATCH,
    BX_SEARCH_WALK_IGNORE_CHECKS,
    BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS,
    BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_CALLS,
    BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_MISSES,
};

bool bx_search_dev_counters_enabled(void);
void bx_search_dev_counters_begin_from_env(void);
void bx_search_dev_counters_reset(void);
void bx_search_dev_counters_note_bytes_read(size_t count);
void bx_search_dev_counters_note_file_opened(void);
void bx_search_dev_counters_note_content_open_call(void);
void bx_search_dev_counters_note_content_close_call(void);
void bx_search_dev_counters_note_content_fstat_call(void);
void bx_search_dev_counters_note_content_fcntl_call(void);
void bx_search_dev_counters_note_content_read(size_t count);
void bx_search_dev_counters_note_content_pread(size_t count);
void bx_search_dev_counters_note_prefix_pread(size_t count);
void bx_search_dev_counters_note_prefix_bytes_rescanned(size_t count);
void bx_search_dev_counters_note_candidate_hit(void);
void bx_search_dev_counters_note_literal_candidate_hit(void);
void bx_search_dev_counters_note_literal_confirm_call(void);
void bx_search_dev_counters_note_literal_match(void);
void bx_search_dev_counters_note_literal_not_found(void);
void bx_search_dev_counters_note_literal_overlap_bytes_scanned(size_t count);
void bx_search_dev_counters_note_literal_cross_chunk_match(void);
void bx_search_dev_counters_note_literal_plan_compile(void);
void bx_search_dev_counters_note_literal_selected_pair_distribution(size_t pair_offset,
                                                                    size_t needle_len);
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
void bx_search_dev_counters_note_literal_rare_pair_probe_call(void);
void bx_search_dev_counters_note_literal_backend_selection(uint64_t requested,
                                                           uint64_t resolved,
                                                           bool avx2_runtime_available,
                                                           bool avx2_target_available,
                                                           bool avx2_eligible_but_not_selected);
void bx_search_dev_counters_note_literal_algo_sse2_call(void);
void bx_search_dev_counters_note_literal_sse2_first_last_call(void);
void bx_search_dev_counters_note_matcher_invocation(void);
void bx_search_dev_counters_note_record_materialized(void);
void bx_search_dev_counters_note_scanner_entry(void);
void bx_search_dev_counters_note_scanner_entry_from_literal_candidate(void);
void bx_search_dev_counters_note_scanner_entry_without_candidate(void);
void bx_search_dev_counters_note_lines_counted(size_t count);
void bx_search_dev_counters_note_line_boundaries_recovered(size_t count);
void bx_search_dev_counters_note_record_expanded(void);
void bx_search_dev_counters_note_plain_line_output(void);
void bx_search_dev_counters_note_context_buffer_entry(void);
void bx_search_dev_counters_note_scanner_plain_prefix_alloc(void);
void bx_search_dev_counters_note_output_line_emitted(void);
void bx_search_dev_counters_note_binary_policy_check(void);
void bx_search_dev_counters_note_walk(enum bx_search_walk_counter counter, uint64_t count);
void bx_search_dev_counters_note_rg_sched(enum bx_search_rg_sched_counter counter,
                                          uint64_t count);
void bx_search_dev_counters_report(FILE *stream);

#endif
