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
    bool batch_debug_enabled;
    atomic_uint_fast64_t batch_debug_next_id;
    atomic_size_t bytes_read;
    atomic_size_t files_opened;
    atomic_uint_fast64_t content_open_calls;
    atomic_uint_fast64_t content_close_calls;
    atomic_uint_fast64_t content_fstat_calls;
    atomic_uint_fast64_t content_fcntl_calls;
    atomic_uint_fast64_t content_read_calls;
    atomic_uint_fast64_t content_read_bytes;
    atomic_uint_fast64_t content_pread_calls;
    atomic_uint_fast64_t content_pread_bytes;
    atomic_uint_fast64_t prefix_pread_calls;
    atomic_uint_fast64_t prefix_pread_bytes;
    atomic_uint_fast64_t prefix_bytes_rescanned;
    atomic_uint_fast64_t transform_prefix_checks;
    atomic_uint_fast64_t binary_prefix_checks;
    atomic_uint_fast64_t files_cut_off_by_binary_prefix;
    atomic_uint_fast64_t candidate_triggered_reopen_calls;
    atomic_uint_fast64_t candidate_triggered_scanner_entries;
    atomic_uint_fast64_t raw_fd_to_scanner_entries;
    atomic_uint_fast64_t raw_fd_to_output_entries;
    atomic_uint_fast64_t raw_fd_to_diagnostic_entries;
    atomic_size_t candidate_hits;
    atomic_size_t literal_candidate_hits;
    atomic_size_t literal_confirm_calls;
    atomic_size_t literal_matches;
    atomic_size_t literal_not_found;
    atomic_size_t literal_overlap_bytes_scanned;
    atomic_size_t literal_cross_chunk_matches;
    atomic_size_t literal_plan_compiles;
    atomic_size_t literal_selected_pair_start;
    atomic_size_t literal_selected_pair_interior;
    atomic_size_t literal_selected_pair_end;
    atomic_size_t literal_algo_empty_calls;
    atomic_size_t literal_algo_byte_calls;
    atomic_size_t literal_algo_pair_calls;
    atomic_size_t literal_algo_short_calls;
    atomic_size_t literal_algo_rare_pair_calls;
    atomic_size_t literal_algo_long_calls;
    atomic_size_t literal_algo_scalar_calls;
    atomic_size_t literal_algo_x86_avx2_calls;
    atomic_size_t literal_algo_arm64_neon_calls;
    atomic_size_t literal_algo_arm64_sve_calls;
    atomic_size_t literal_algo_memmem_calls;
    atomic_size_t literal_bytes_scanned;
    atomic_size_t literal_rare_pair_probe_calls;
    atomic_size_t literal_pair_mask_nonzero;
    atomic_uint_fast64_t literal_backend_requested;
    atomic_uint_fast64_t literal_backend_resolved;
    atomic_uint_fast64_t literal_avx2_runtime_available;
    atomic_uint_fast64_t literal_avx2_target_available;
    atomic_uint_fast64_t literal_avx2_eligible_but_not_selected;
    atomic_size_t literal_algo_sse2_calls;
    atomic_size_t literal_sse2_first_last_calls;
    atomic_size_t matcher_invocations;
    atomic_size_t records_materialized;
    atomic_size_t scanner_entries;
    atomic_size_t scanner_entries_from_literal_candidate;
    atomic_size_t scanner_entries_without_candidate;
    atomic_size_t lines_counted;
    atomic_size_t line_boundaries_recovered;
    atomic_size_t candidate_triggered_line_recovery_reread_bytes;
    atomic_size_t records_expanded;
    atomic_size_t plain_line_outputs;
    atomic_size_t context_buffer_entries;
    atomic_size_t scanner_plain_prefix_allocs;
    atomic_size_t output_lines_emitted;
    atomic_uint_fast64_t binary_policy_checks;
    atomic_uint_fast64_t display_path_borrows;
    atomic_uint_fast64_t display_path_copies;
    atomic_uint_fast64_t display_path_copy_bytes;
    atomic_uint_fast64_t walk_dirents_seen;
    atomic_uint_fast64_t walk_getdents64_calls;
    atomic_uint_fast64_t walk_getdents64_bytes;
    atomic_uint_fast64_t walk_dirs_seen;
    atomic_uint_fast64_t walk_files_seen;
    atomic_uint_fast64_t walk_symlinks_seen;
    atomic_uint_fast64_t walk_unknown_dtype_seen;
    atomic_uint_fast64_t walk_stat_calls;
    atomic_uint_fast64_t walk_fstat_calls;
    atomic_uint_fast64_t walk_lstat_calls;
    atomic_uint_fast64_t walk_fstatat_calls;
    atomic_uint_fast64_t walk_stat_reason_unknown_dtype;
    atomic_uint_fast64_t walk_stat_reason_symlink_policy;
    atomic_uint_fast64_t walk_stat_reason_traversal_policy;
    atomic_uint_fast64_t walk_stat_reason_metadata_filter;
    atomic_uint_fast64_t walk_stat_reason_max_filesize;
    atomic_uint_fast64_t walk_stat_reason_min_filesize;
    atomic_uint_fast64_t walk_stat_reason_type;
    atomic_uint_fast64_t walk_stat_reason_sort;
    atomic_uint_fast64_t walk_stat_reason_metadata_output;
    atomic_uint_fast64_t walk_stat_reason_explicit_operand;
    atomic_uint_fast64_t walk_openat_calls;
    atomic_uint_fast64_t walk_path_join_calls;
    atomic_uint_fast64_t walk_path_push_calls;
    atomic_uint_fast64_t walk_path_push_ns;
    atomic_uint_fast64_t walk_path_pop_calls;
    atomic_uint_fast64_t walk_path_pop_ns;
    atomic_uint_fast64_t walk_path_allocs;
    atomic_uint_fast64_t walk_path_copies_before_match;
    atomic_uint_fast64_t walk_dir_bucket_tiny_dirs;
    atomic_uint_fast64_t walk_dir_bucket_tiny_entries;
    atomic_uint_fast64_t walk_dir_bucket_tiny_ns;
    atomic_uint_fast64_t walk_dir_bucket_small_dirs;
    atomic_uint_fast64_t walk_dir_bucket_small_entries;
    atomic_uint_fast64_t walk_dir_bucket_small_ns;
    atomic_uint_fast64_t walk_dir_bucket_medium_dirs;
    atomic_uint_fast64_t walk_dir_bucket_medium_entries;
    atomic_uint_fast64_t walk_dir_bucket_medium_ns;
    atomic_uint_fast64_t walk_dir_bucket_huge_dirs;
    atomic_uint_fast64_t walk_dir_bucket_huge_entries;
    atomic_uint_fast64_t walk_dir_bucket_huge_ns;
    atomic_uint_fast64_t walk_ignore_checks;
    atomic_uint_fast64_t walk_ignore_literal_basename_checks;
    atomic_uint_fast64_t walk_ignore_literal_basename_rejects;
    atomic_uint_fast64_t walk_ignore_literal_extension_checks;
    atomic_uint_fast64_t walk_ignore_literal_extension_rejects;
    atomic_uint_fast64_t walk_ignore_anchored_prefix_checks;
    atomic_uint_fast64_t walk_ignore_anchored_prefix_rejects;
    atomic_uint_fast64_t walk_ignore_basename_only_fast_paths;
    atomic_uint_fast64_t walk_ignore_no_generic_glob_fast_paths;
    atomic_uint_fast64_t walk_ignore_builtin_checks;
    atomic_uint_fast64_t walk_ignore_builtin_rejects;
    atomic_uint_fast64_t walk_ignore_gitignore_checks;
    atomic_uint_fast64_t walk_ignore_gitignore_rejects;
    atomic_uint_fast64_t walk_ignore_dotignore_checks;
    atomic_uint_fast64_t walk_ignore_dotignore_rejects;
    atomic_uint_fast64_t walk_ignore_glob_fallbacks;
    atomic_uint_fast64_t walk_ignore_generic_glob_checks;
    atomic_uint_fast64_t walk_ignore_generic_glob_rejects;
    atomic_uint_fast64_t walk_ignore_git_root_lstat_calls;
    atomic_uint_fast64_t walk_ignore_git_root_lstat_misses;
    atomic_uint_fast64_t walk_filter_ns;
    atomic_uint_fast64_t walk_ignore_state_pushes;
    atomic_uint_fast64_t walk_ignore_state_inline_frames;
    atomic_uint_fast64_t walk_ignore_state_fast_paths;
    atomic_uint_fast64_t walk_ignore_state_ns;
    atomic_uint_fast64_t walk_filter_hidden_policy_checks;
    atomic_uint_fast64_t walk_filter_hidden_policy_rejects;
    atomic_uint_fast64_t walk_filter_type_policy_checks;
    atomic_uint_fast64_t walk_filter_type_policy_rejects;
    atomic_uint_fast64_t walk_filter_cli_glob_checks;
    atomic_uint_fast64_t walk_filter_cli_glob_rejects;
    atomic_uint_fast64_t walk_filter_rejected_entries;
    atomic_uint_fast64_t walk_filter_rejected_dirs;
    atomic_uint_fast64_t files_seen;
    atomic_uint_fast64_t dirs_seen;
    atomic_uint_fast64_t global_pool_submits;
    atomic_uint_fast64_t global_pool_pops;
    atomic_uint_fast64_t global_queue_lock_acquires;
    atomic_uint_fast64_t global_queue_cond_wakeups;
    atomic_uint_fast64_t worker_slot_lock_acquires;
    atomic_uint_fast64_t worker_wakeups;
    atomic_uint_fast64_t path_bytes_copied;
    atomic_uint_fast64_t path_copies_before_match;
    atomic_uint_fast64_t search_batch_files;
    atomic_uint_fast64_t search_batch_path_bytes;
    atomic_uint_fast64_t search_batch_allocs;
    atomic_uint_fast64_t search_batch_storage_reallocs;
    atomic_uint_fast64_t search_batch_lifetime_empty;
    atomic_uint_fast64_t search_batches_queued;
    atomic_uint_fast64_t search_batches_searched;
    atomic_uint_fast64_t search_batches_empty;
    atomic_uint_fast64_t memstreams_opened;
    atomic_uint_fast64_t output_records_submitted;
    atomic_uint_fast64_t diagnostic_records_submitted;
    atomic_uint_fast64_t match_records_submitted;
    atomic_uint_fast64_t ordered_output_records;
    atomic_uint_fast64_t unordered_output_flushes;
    atomic_uint_fast64_t skipped_output_seqs;
    atomic_uint_fast64_t local_files_searched;
    atomic_uint_fast64_t stolen_files_searched;
    atomic_uint_fast64_t local_dirs_walked;
    atomic_uint_fast64_t stolen_dirs_walked;
    atomic_uint_fast64_t queued_output_batches;
    atomic_uint_fast64_t empty_output_batches;
    atomic_uint_fast64_t output_batch_records;
    atomic_uint_fast64_t output_batch_stdout_bytes;
    atomic_uint_fast64_t output_batch_stderr_bytes;
    atomic_uint_fast64_t worker_subtrees_donated;
    atomic_uint_fast64_t worker_subtrees_stolen;
    atomic_uint_fast64_t worker_donated_dir_opened;
    atomic_uint_fast64_t worker_donated_dir_open_failures;
    atomic_uint_fast64_t worker_donated_dir_owned_walks;
    atomic_uint_fast64_t worker_donated_dir_reopen_fallbacks;
};

static struct bx_search_dev_counters current_dev_counters = {0};

bool bx_search_dev_counters_enabled(void) {
    return current_dev_counters.enabled;
}

void bx_search_dev_counters_begin_from_env(void) {
    bx_search_dev_counters_reset();

    const char *batch_debug = getenv("BX_SEARCH_BATCH_DEBUG");
    if (batch_debug && *batch_debug && strcmp(batch_debug, "0") != 0)
        current_dev_counters.batch_debug_enabled = true;

    const char *value = getenv("BX_SEARCH_DEV_COUNTERS");
    if (!value || !*value || strcmp(value, "0") == 0)
        return;

    current_dev_counters.enabled = true;
}

void bx_search_dev_counters_reset(void) {
    current_dev_counters.enabled = false;
    current_dev_counters.batch_debug_enabled = false;
    atomic_store_explicit(&current_dev_counters.batch_debug_next_id, 1u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.bytes_read, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.files_opened, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_open_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_close_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_fstat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_fcntl_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_read_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_read_bytes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_pread_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.content_pread_bytes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.prefix_pread_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.prefix_pread_bytes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.prefix_bytes_rescanned, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.transform_prefix_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.binary_prefix_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.files_cut_off_by_binary_prefix, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.candidate_triggered_reopen_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.candidate_triggered_scanner_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.raw_fd_to_scanner_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.raw_fd_to_output_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.raw_fd_to_diagnostic_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.candidate_hits, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_candidate_hits, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_confirm_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_matches, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_not_found, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_overlap_bytes_scanned, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_cross_chunk_matches, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_plan_compiles, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_selected_pair_start, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_selected_pair_interior, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_selected_pair_end, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_empty_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_byte_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_pair_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_short_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_rare_pair_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_long_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_scalar_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_x86_avx2_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_arm64_neon_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_arm64_sve_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_memmem_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_bytes_scanned, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_rare_pair_probe_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_pair_mask_nonzero, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_backend_requested, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_backend_resolved, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_avx2_runtime_available, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_avx2_target_available, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_avx2_eligible_but_not_selected, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_algo_sse2_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_sse2_first_last_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.matcher_invocations, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.records_materialized, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.scanner_entries, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.scanner_entries_from_literal_candidate, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.scanner_entries_without_candidate, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.lines_counted, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.line_boundaries_recovered, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.candidate_triggered_line_recovery_reread_bytes,
                          0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.records_expanded, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.plain_line_outputs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.context_buffer_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.scanner_plain_prefix_allocs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_lines_emitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.binary_policy_checks, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.display_path_borrows, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.display_path_copies, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.display_path_copy_bytes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dirents_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_getdents64_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_getdents64_bytes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dirs_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_files_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_symlinks_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_unknown_dtype_seen, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_fstat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_lstat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_fstatat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_unknown_dtype, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_symlink_policy, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_traversal_policy, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_metadata_filter, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_max_filesize, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_min_filesize, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_type, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_sort, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_metadata_output, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_stat_reason_explicit_operand, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_openat_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_join_calls, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_push_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_push_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_pop_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_pop_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_allocs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_path_copies_before_match, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_tiny_dirs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_tiny_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_tiny_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_small_dirs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_small_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_small_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_medium_dirs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_medium_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_medium_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_huge_dirs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_huge_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_dir_bucket_huge_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_checks, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_literal_basename_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_literal_basename_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_literal_extension_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_literal_extension_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_anchored_prefix_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_anchored_prefix_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_basename_only_fast_paths, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_no_generic_glob_fast_paths, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_builtin_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_builtin_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_gitignore_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_gitignore_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_dotignore_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_dotignore_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_glob_fallbacks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_generic_glob_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_generic_glob_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_git_root_lstat_calls, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_git_root_lstat_misses, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_state_pushes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_state_inline_frames, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_state_fast_paths, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_ignore_state_ns, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_hidden_policy_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_hidden_policy_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_type_policy_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_type_policy_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_cli_glob_checks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_cli_glob_rejects, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_rejected_entries, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.walk_filter_rejected_dirs, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.files_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.dirs_seen, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_pool_submits, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_pool_pops, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_queue_lock_acquires, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.global_queue_cond_wakeups, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_slot_lock_acquires, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_wakeups, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.path_bytes_copied, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.path_copies_before_match, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batch_files, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batch_path_bytes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batch_allocs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batch_storage_reallocs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batch_lifetime_empty, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batches_queued, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batches_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.search_batches_empty, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.memstreams_opened, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_records_submitted, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.diagnostic_records_submitted, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.match_records_submitted, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.ordered_output_records, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.unordered_output_flushes, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.skipped_output_seqs, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.local_files_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.stolen_files_searched, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.local_dirs_walked, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.stolen_dirs_walked, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.queued_output_batches, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.empty_output_batches, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_batch_records, 0u, memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_batch_stdout_bytes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.output_batch_stderr_bytes, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_subtrees_donated, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_subtrees_stolen, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_donated_dir_opened, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_donated_dir_open_failures, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_donated_dir_owned_walks, 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.worker_donated_dir_reopen_fallbacks, 0u,
                          memory_order_relaxed);
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

void bx_search_dev_counters_note_content_open_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_open_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_content_close_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_close_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_content_fstat_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_fstat_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_content_fcntl_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_fcntl_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_content_read(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_read_calls, 1u,
                              memory_order_relaxed);
    if (count == 0u)
        return;
    atomic_fetch_add_explicit(&current_dev_counters.content_read_bytes, count,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&current_dev_counters.bytes_read, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_content_pread(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.content_pread_calls, 1u,
                              memory_order_relaxed);
    if (count == 0u)
        return;
    atomic_fetch_add_explicit(&current_dev_counters.content_pread_bytes, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_prefix_pread(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.prefix_pread_calls, 1u,
                              memory_order_relaxed);
    if (count == 0u)
        return;
    atomic_fetch_add_explicit(&current_dev_counters.prefix_pread_bytes, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_prefix_bytes_rescanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.prefix_bytes_rescanned, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_transform_prefix_check(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.transform_prefix_checks, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_binary_prefix_check(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.binary_prefix_checks, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_file_cut_off_by_binary_prefix(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.files_cut_off_by_binary_prefix, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_candidate_triggered_reopen_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.candidate_triggered_reopen_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_candidate_triggered_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.candidate_triggered_scanner_entries, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_raw_fd_to_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.raw_fd_to_scanner_entries, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_raw_fd_to_output_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.raw_fd_to_output_entries, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_raw_fd_to_diagnostic_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.raw_fd_to_diagnostic_entries, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.candidate_hits, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.candidate_hits, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&current_dev_counters.literal_candidate_hits, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_confirm_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_confirm_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_match(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_matches, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_not_found(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_not_found, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_overlap_bytes_scanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_overlap_bytes_scanned, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_cross_chunk_match(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_cross_chunk_matches, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_plan_compile(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_plan_compiles, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_selected_pair_distribution(size_t pair_offset,
                                                                    size_t needle_len) {
    if (!current_dev_counters.enabled || needle_len < 2u)
        return;

    if (pair_offset == 0u) {
        atomic_fetch_add_explicit(&current_dev_counters.literal_selected_pair_start, 1u,
                                  memory_order_relaxed);
        return;
    }
    if (pair_offset + 2u == needle_len) {
        atomic_fetch_add_explicit(&current_dev_counters.literal_selected_pair_end, 1u,
                                  memory_order_relaxed);
        return;
    }
    atomic_fetch_add_explicit(&current_dev_counters.literal_selected_pair_interior, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_empty_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_empty_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_byte_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_byte_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_pair_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_pair_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_short_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_short_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_rare_pair_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_rare_pair_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_long_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_long_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_scalar_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_scalar_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_x86_avx2_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_x86_avx2_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_arm64_neon_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_arm64_neon_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_arm64_sve_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_arm64_sve_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_algo_memmem_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_memmem_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_bytes_scanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_bytes_scanned, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_rare_pair_probe_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_rare_pair_probe_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_pair_mask_nonzero(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_pair_mask_nonzero, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_backend_selection(
    uint64_t requested,
    uint64_t resolved,
    bool avx2_runtime_available,
    bool avx2_target_available,
    bool avx2_eligible_but_not_selected) {
    if (!current_dev_counters.enabled)
        return;

    atomic_store_explicit(&current_dev_counters.literal_backend_requested, requested,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_backend_resolved, resolved,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_avx2_runtime_available,
                          avx2_runtime_available ? 1u : 0u,
                          memory_order_relaxed);
    atomic_store_explicit(&current_dev_counters.literal_avx2_target_available,
                          avx2_target_available ? 1u : 0u,
                          memory_order_relaxed);
    if (avx2_eligible_but_not_selected) {
        atomic_fetch_add_explicit(
            &current_dev_counters.literal_avx2_eligible_but_not_selected,
            1u,
            memory_order_relaxed);
    }
}

void bx_search_dev_counters_note_literal_algo_sse2_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_algo_sse2_calls, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_literal_sse2_first_last_call(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.literal_sse2_first_last_calls, 1u,
                              memory_order_relaxed);
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

void bx_search_dev_counters_note_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.scanner_entries, 1u, memory_order_relaxed);
}

void bx_search_dev_counters_note_scanner_entry_from_literal_candidate(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.scanner_entries_from_literal_candidate, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_scanner_entry_without_candidate(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.scanner_entries_without_candidate, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_lines_counted(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.lines_counted, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_line_boundaries_recovered(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.line_boundaries_recovered, count,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_candidate_triggered_line_recovery_reread(size_t bytes) {
    if (!current_dev_counters.enabled || bytes == 0u)
        return;

    atomic_fetch_add_explicit(
        &current_dev_counters.candidate_triggered_line_recovery_reread_bytes,
        bytes,
        memory_order_relaxed);
}

void bx_search_dev_counters_note_record_expanded(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.records_expanded, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_plain_line_output(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.plain_line_outputs, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_context_buffer_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.context_buffer_entries, 1u,
                              memory_order_relaxed);
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

void bx_search_dev_counters_note_binary_policy_check(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.binary_policy_checks, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_display_path_borrow(void) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.display_path_borrows, 1u,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_display_path_copy(size_t bytes) {
    if (!current_dev_counters.enabled)
        return;

    atomic_fetch_add_explicit(&current_dev_counters.display_path_copies, 1u,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&current_dev_counters.display_path_copy_bytes, bytes,
                              memory_order_relaxed);
}

void bx_search_dev_counters_note_walk(enum bx_search_walk_counter counter,
                                      uint64_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    switch (counter) {
    case BX_SEARCH_WALK_DIRENTS_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dirents_seen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_GETDENTS64_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_getdents64_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_GETDENTS64_BYTES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_getdents64_bytes, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIRS_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dirs_seen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILES_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.walk_files_seen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_SYMLINKS_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.walk_symlinks_seen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_UNKNOWN_DTYPE_SEEN:
        atomic_fetch_add_explicit(&current_dev_counters.walk_unknown_dtype_seen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FSTAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_fstat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_LSTAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_lstat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FSTATAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_fstatat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_UNKNOWN_DTYPE:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_unknown_dtype, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_SYMLINK_POLICY:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_symlink_policy, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_TRAVERSAL_POLICY:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_traversal_policy, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_METADATA_FILTER:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_metadata_filter, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_MAX_FILESIZE:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_max_filesize, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_MIN_FILESIZE:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_min_filesize, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_TYPE:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_type, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_SORT:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_sort, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_METADATA_OUTPUT:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_metadata_output, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND:
        atomic_fetch_add_explicit(&current_dev_counters.walk_stat_reason_explicit_operand, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_OPENAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_openat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_JOIN_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_join_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_PUSH_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_push_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_PUSH_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_push_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_POP_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_pop_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_POP_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_pop_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_ALLOCS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_allocs, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_PATH_COPIES_BEFORE_MATCH:
        atomic_fetch_add_explicit(&current_dev_counters.walk_path_copies_before_match, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_DIRS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_tiny_dirs, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_ENTRIES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_tiny_entries, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_tiny_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_DIRS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_small_dirs, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_ENTRIES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_small_entries, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_small_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_DIRS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_medium_dirs, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_ENTRIES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_medium_entries, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_medium_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_DIRS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_huge_dirs, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_ENTRIES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_huge_entries, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_dir_bucket_huge_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_checks, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_literal_basename_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_literal_basename_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_literal_extension_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_literal_extension_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_anchored_prefix_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_anchored_prefix_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_BASENAME_ONLY_FAST_PATHS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_basename_only_fast_paths,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_NO_GENERIC_GLOB_FAST_PATHS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_no_generic_glob_fast_paths,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_BUILTIN_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_builtin_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_BUILTIN_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_builtin_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GITIGNORE_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_gitignore_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GITIGNORE_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_gitignore_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_DOTIGNORE_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_dotignore_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_DOTIGNORE_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_dotignore_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_glob_fallbacks, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_generic_glob_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_generic_glob_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_CALLS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_git_root_lstat_calls, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_MISSES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_git_root_lstat_misses, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_PUSHES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_state_pushes, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_INLINE_FRAMES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_state_inline_frames, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_FAST_PATHS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_state_fast_paths, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_NS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_ignore_state_ns, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_hidden_policy_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_hidden_policy_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_TYPE_POLICY_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_type_policy_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_type_policy_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_CLI_GLOB_CHECKS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_cli_glob_checks,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_cli_glob_rejects,
                                  count, memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_REJECTED_ENTRIES:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_rejected_entries, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_WALK_FILTER_REJECTED_DIRS:
        atomic_fetch_add_explicit(&current_dev_counters.walk_filter_rejected_dirs, count,
                                  memory_order_relaxed);
        return;
    }
}

void bx_search_dev_counters_note_walk_stat_call(enum bx_search_walk_counter reason) {
    if (!current_dev_counters.enabled)
        return;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_STAT_CALLS, 1u);
    bx_search_dev_counters_note_walk(reason, 1u);
}

void bx_search_dev_counters_note_walk_fstat_call(enum bx_search_walk_counter reason) {
    if (!current_dev_counters.enabled)
        return;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FSTAT_CALLS, 1u);
    bx_search_dev_counters_note_walk(reason, 1u);
}

void bx_search_dev_counters_note_walk_lstat_call(enum bx_search_walk_counter reason) {
    if (!current_dev_counters.enabled)
        return;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_LSTAT_CALLS, 1u);
    bx_search_dev_counters_note_walk(reason, 1u);
}

void bx_search_dev_counters_note_walk_fstatat_call(enum bx_search_walk_counter reason) {
    if (!current_dev_counters.enabled)
        return;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FSTATAT_CALLS, 1u);
    bx_search_dev_counters_note_walk(reason, 1u);
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
    case BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_LOCK_ACQUIRES:
        atomic_fetch_add_explicit(&current_dev_counters.global_queue_lock_acquires, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_COND_WAKEUPS:
        atomic_fetch_add_explicit(&current_dev_counters.global_queue_cond_wakeups, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SLOT_LOCK_ACQUIRES:
        atomic_fetch_add_explicit(&current_dev_counters.worker_slot_lock_acquires, count,
                                  memory_order_relaxed);
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
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_FILES:
        atomic_fetch_add_explicit(&current_dev_counters.search_batch_files, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_PATH_BYTES:
        atomic_fetch_add_explicit(&current_dev_counters.search_batch_path_bytes, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_ALLOCS:
        atomic_fetch_add_explicit(&current_dev_counters.search_batch_allocs, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_STORAGE_REALLOCS:
        atomic_fetch_add_explicit(&current_dev_counters.search_batch_storage_reallocs, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_LIFETIME_EMPTY:
        atomic_fetch_add_explicit(&current_dev_counters.search_batch_lifetime_empty, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_QUEUED:
        atomic_fetch_add_explicit(&current_dev_counters.search_batches_queued, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_SEARCHED:
        atomic_fetch_add_explicit(&current_dev_counters.search_batches_searched, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_EMPTY:
        atomic_fetch_add_explicit(&current_dev_counters.search_batches_empty, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED:
        atomic_fetch_add_explicit(&current_dev_counters.memstreams_opened, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_RECORDS_SUBMITTED:
        atomic_fetch_add_explicit(&current_dev_counters.output_records_submitted, count, memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_DIAGNOSTIC_RECORDS_SUBMITTED:
        atomic_fetch_add_explicit(&current_dev_counters.diagnostic_records_submitted, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_MATCH_RECORDS_SUBMITTED:
        atomic_fetch_add_explicit(&current_dev_counters.match_records_submitted, count,
                                  memory_order_relaxed);
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
    case BX_SEARCH_RG_SCHED_QUEUED_OUTPUT_BATCHES:
        atomic_fetch_add_explicit(&current_dev_counters.queued_output_batches, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_EMPTY_OUTPUT_BATCHES:
        atomic_fetch_add_explicit(&current_dev_counters.empty_output_batches, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_RECORDS:
        atomic_fetch_add_explicit(&current_dev_counters.output_batch_records, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_STDOUT_BYTES:
        atomic_fetch_add_explicit(&current_dev_counters.output_batch_stdout_bytes, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_STDERR_BYTES:
        atomic_fetch_add_explicit(&current_dev_counters.output_batch_stderr_bytes, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SUBTREES_DONATED:
        atomic_fetch_add_explicit(&current_dev_counters.worker_subtrees_donated, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SUBTREES_STOLEN:
        atomic_fetch_add_explicit(&current_dev_counters.worker_subtrees_stolen, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPENED:
        atomic_fetch_add_explicit(&current_dev_counters.worker_donated_dir_opened, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPEN_FAILURES:
        atomic_fetch_add_explicit(&current_dev_counters.worker_donated_dir_open_failures, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OWNED_WALKS:
        atomic_fetch_add_explicit(&current_dev_counters.worker_donated_dir_owned_walks, count,
                                  memory_order_relaxed);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_REOPEN_FALLBACKS:
        atomic_fetch_add_explicit(&current_dev_counters.worker_donated_dir_reopen_fallbacks,
                                  count, memory_order_relaxed);
        return;
    }
}

uint64_t bx_search_dev_batch_debug_next_id(void) {
    if (!current_dev_counters.batch_debug_enabled)
        return 0u;
    return (uint64_t)atomic_fetch_add_explicit(&current_dev_counters.batch_debug_next_id,
                                               1u,
                                               memory_order_relaxed);
}

void bx_search_dev_batch_debug_search(const char *source,
                                      const char *event,
                                      uint64_t id,
                                      uint64_t files,
                                      uint64_t path_bytes) {
    if (!current_dev_counters.batch_debug_enabled)
        return;

    fprintf(stderr,
            "bx-search-batch-lifecycle: kind=search source=%s event=%s id=%" PRIu64
            " files=%" PRIu64 " path_bytes=%" PRIu64 "\n",
            source ? source : "?",
            event ? event : "?",
            id,
            files,
            path_bytes);
}

void bx_search_dev_batch_debug_output(const char *source,
                                      const char *event,
                                      uint64_t id,
                                      uint64_t stdout_bytes,
                                      uint64_t stderr_bytes,
                                      bool match_output,
                                      bool diagnostic_output,
                                      bool empty) {
    if (!current_dev_counters.batch_debug_enabled)
        return;

    fprintf(stderr,
            "bx-search-batch-lifecycle: kind=output source=%s event=%s id=%" PRIu64
            " stdout_bytes=%" PRIu64 " stderr_bytes=%" PRIu64
            " match=%u diagnostic=%u empty=%u\n",
            source ? source : "?",
            event ? event : "?",
            id,
            stdout_bytes,
            stderr_bytes,
            match_output ? 1u : 0u,
            diagnostic_output ? 1u : 0u,
            empty ? 1u : 0u);
}

void bx_search_dev_counters_report(FILE *stream) {
    if (!current_dev_counters.enabled || !stream)
        return;

    fprintf(stream,
            "bx-search-dev-counters: bytes_read=%zu files_opened=%zu "
            "content_open_calls=%" PRIuMAX " content_close_calls=%" PRIuMAX " content_fstat_calls=%" PRIuMAX " content_fcntl_calls=%" PRIuMAX " "
            "content_read_calls=%" PRIuMAX " content_read_bytes=%" PRIuMAX " content_pread_calls=%" PRIuMAX " content_pread_bytes=%" PRIuMAX " "
            "prefix_pread_calls=%" PRIuMAX " prefix_pread_bytes=%" PRIuMAX " prefix_bytes_rescanned=%" PRIuMAX " transform_prefix_checks=%" PRIuMAX " binary_prefix_checks=%" PRIuMAX " files_cut_off_by_binary_prefix=%" PRIuMAX " candidate_triggered_reopen_calls=%" PRIuMAX " candidate_triggered_scanner_entries=%" PRIuMAX " "
            "raw_fd_to_scanner_entries=%" PRIuMAX " raw_fd_to_output_entries=%" PRIuMAX " raw_fd_to_diagnostic_entries=%" PRIuMAX " "
            "candidate_hits=%zu literal_candidate_hits=%zu literal_confirm_calls=%zu literal_matches=%zu literal_not_found=%zu literal_overlap_bytes_scanned=%zu literal_cross_chunk_matches=%zu literal_plan_compiles=%zu literal_selected_pair_start=%zu literal_selected_pair_interior=%zu literal_selected_pair_end=%zu literal_algo_empty_calls=%zu literal_algo_byte_calls=%zu literal_algo_pair_calls=%zu literal_algo_short_calls=%zu literal_algo_rare_pair_calls=%zu literal_algo_long_calls=%zu literal_algo_scalar_calls=%zu literal_algo_x86_avx2_calls=%zu literal_algo_arm64_neon_calls=%zu literal_algo_arm64_sve_calls=%zu literal_algo_memmem_calls=%zu literal_bytes_scanned=%zu literal_rare_pair_probe_calls=%zu literal_pair_mask_nonzero=%zu "
            "literal_backend_requested=%" PRIuMAX " literal_backend_resolved=%" PRIuMAX " literal_avx2_runtime_available=%" PRIuMAX " literal_avx2_target_available=%" PRIuMAX " literal_avx2_eligible_but_not_selected=%" PRIuMAX " "
            "literal_algo_sse2_calls=%zu literal_sse2_first_last_calls=%zu matcher_invocations=%zu records_materialized=%zu scanner_entries=%zu scanner_entries_from_literal_candidate=%zu scanner_entries_without_candidate=%zu lines_counted=%zu line_boundaries_recovered=%zu candidate_triggered_line_recovery_reread_bytes=%zu records_expanded=%zu plain_line_outputs=%zu context_buffer_entries=%zu scanner_plain_prefix_allocs=%zu output_lines_emitted=%zu "
            "binary_policy_checks=%" PRIuMAX " display_path_borrows=%" PRIuMAX " display_path_copies=%" PRIuMAX " display_path_copy_bytes=%" PRIuMAX " ",
            atomic_load_explicit(&current_dev_counters.bytes_read, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.files_opened, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_open_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_close_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_fstat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_fcntl_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_read_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_read_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_pread_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.content_pread_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.prefix_pread_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.prefix_pread_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.prefix_bytes_rescanned,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.transform_prefix_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.binary_prefix_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.files_cut_off_by_binary_prefix,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.candidate_triggered_reopen_calls,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.candidate_triggered_scanner_entries,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.raw_fd_to_scanner_entries,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.raw_fd_to_output_entries,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.raw_fd_to_diagnostic_entries,
                memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.candidate_hits, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_candidate_hits,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_confirm_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_matches,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_not_found,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_overlap_bytes_scanned,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_cross_chunk_matches,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_plan_compiles,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_selected_pair_start,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_selected_pair_interior,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_selected_pair_end,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_empty_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_byte_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_pair_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_short_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_rare_pair_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_long_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_scalar_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_x86_avx2_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_arm64_neon_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_arm64_sve_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_memmem_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_bytes_scanned,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_rare_pair_probe_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_pair_mask_nonzero,
                                 memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.literal_backend_requested,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.literal_backend_resolved,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.literal_avx2_runtime_available,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.literal_avx2_target_available,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.literal_avx2_eligible_but_not_selected,
                memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_algo_sse2_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.literal_sse2_first_last_calls,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.matcher_invocations, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.records_materialized, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.scanner_entries, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.scanner_entries_from_literal_candidate,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.scanner_entries_without_candidate,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.lines_counted, memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.line_boundaries_recovered,
                                 memory_order_relaxed),
            atomic_load_explicit(
                &current_dev_counters.candidate_triggered_line_recovery_reread_bytes,
                memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.records_expanded,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.plain_line_outputs,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.context_buffer_entries,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.scanner_plain_prefix_allocs,
                                 memory_order_relaxed),
            atomic_load_explicit(&current_dev_counters.output_lines_emitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.binary_policy_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.display_path_borrows,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.display_path_copies,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.display_path_copy_bytes,
                                            memory_order_relaxed));

    fprintf(stream,
            "walk_dirents_seen=%" PRIuMAX " walk_getdents64_calls=%" PRIuMAX " walk_getdents64_bytes=%" PRIuMAX " walk_dirs_seen=%" PRIuMAX " walk_files_seen=%" PRIuMAX " walk_symlinks_seen=%" PRIuMAX " walk_unknown_dtype_seen=%" PRIuMAX " "
            "walk_stat_calls=%" PRIuMAX " walk_fstat_calls=%" PRIuMAX " walk_lstat_calls=%" PRIuMAX " walk_fstatat_calls=%" PRIuMAX " walk_stat_reason_unknown_dtype=%" PRIuMAX " walk_stat_reason_symlink_policy=%" PRIuMAX " walk_stat_reason_traversal_policy=%" PRIuMAX " "
            "walk_stat_reason_metadata_filter=%" PRIuMAX " walk_stat_reason_max_filesize=%" PRIuMAX " walk_stat_reason_min_filesize=%" PRIuMAX " walk_stat_reason_type=%" PRIuMAX " walk_stat_reason_sort=%" PRIuMAX " walk_stat_reason_metadata_output=%" PRIuMAX " walk_stat_reason_explicit_operand=%" PRIuMAX " walk_openat_calls=%" PRIuMAX " walk_path_join_calls=%" PRIuMAX " walk_path_push_calls=%" PRIuMAX " walk_path_push_ns=%" PRIuMAX " walk_path_pop_calls=%" PRIuMAX " walk_path_pop_ns=%" PRIuMAX " walk_path_allocs=%" PRIuMAX " "
            "walk_path_copies_before_match=%" PRIuMAX " walk_ignore_checks=%" PRIuMAX " walk_ignore_literal_basename_checks=%" PRIuMAX " walk_ignore_literal_basename_rejects=%" PRIuMAX " walk_ignore_literal_extension_checks=%" PRIuMAX " walk_ignore_literal_extension_rejects=%" PRIuMAX " walk_ignore_anchored_prefix_checks=%" PRIuMAX " walk_ignore_anchored_prefix_rejects=%" PRIuMAX " walk_ignore_basename_only_fast_paths=%" PRIuMAX " walk_ignore_no_generic_glob_fast_paths=%" PRIuMAX " "
            "walk_ignore_builtin_checks=%" PRIuMAX " walk_ignore_builtin_rejects=%" PRIuMAX " walk_ignore_gitignore_checks=%" PRIuMAX " walk_ignore_gitignore_rejects=%" PRIuMAX " walk_ignore_dotignore_checks=%" PRIuMAX " walk_ignore_dotignore_rejects=%" PRIuMAX " walk_ignore_glob_fallbacks=%" PRIuMAX " walk_ignore_generic_glob_checks=%" PRIuMAX " walk_ignore_generic_glob_rejects=%" PRIuMAX " "
            "walk_ignore_git_root_lstat_calls=%" PRIuMAX " walk_ignore_git_root_lstat_misses=%" PRIuMAX " walk_filter_ns=%" PRIuMAX " walk_ignore_state_pushes=%" PRIuMAX " walk_ignore_state_inline_frames=%" PRIuMAX " walk_ignore_state_fast_paths=%" PRIuMAX " walk_ignore_state_ns=%" PRIuMAX " walk_filter_hidden_policy_checks=%" PRIuMAX " walk_filter_hidden_policy_rejects=%" PRIuMAX " walk_filter_type_policy_checks=%" PRIuMAX " walk_filter_type_policy_rejects=%" PRIuMAX " walk_filter_cli_glob_checks=%" PRIuMAX " walk_filter_cli_glob_rejects=%" PRIuMAX " walk_filter_rejected_entries=%" PRIuMAX " walk_filter_rejected_dirs=%" PRIuMAX " "
            "files_seen=%" PRIuMAX " dirs_seen=%" PRIuMAX " global_pool_submits=%" PRIuMAX " global_pool_pops=%" PRIuMAX " global_queue_lock_acquires=%" PRIuMAX " global_queue_cond_wakeups=%" PRIuMAX " worker_slot_lock_acquires=%" PRIuMAX " worker_wakeups=%" PRIuMAX " "
            "path_bytes_copied=%" PRIuMAX " path_copies_before_match=%" PRIuMAX " search_batch_files=%" PRIuMAX " search_batch_path_bytes=%" PRIuMAX " search_batch_allocs=%" PRIuMAX " search_batch_storage_reallocs=%" PRIuMAX " search_batch_lifetime_empty=%" PRIuMAX " queued_search_batches=%" PRIuMAX " searched_search_batches=%" PRIuMAX " empty_search_batches=%" PRIuMAX " batches_built=%" PRIuMAX " batches_searched=%" PRIuMAX " empty_batches=%" PRIuMAX " "
            "memstreams_opened=%" PRIuMAX " output_records_submitted=%" PRIuMAX " diagnostic_records_submitted=%" PRIuMAX " match_records_submitted=%" PRIuMAX " ordered_output_records=%" PRIuMAX " unordered_output_flushes=%" PRIuMAX " "
            "skipped_output_seqs=%" PRIuMAX " local_files_searched=%" PRIuMAX " stolen_files_searched=%" PRIuMAX " local_dirs_walked=%" PRIuMAX " stolen_dirs_walked=%" PRIuMAX " "
            "queued_output_batches=%" PRIuMAX " empty_output_batches=%" PRIuMAX " output_batch_records=%" PRIuMAX " output_batch_stdout_bytes=%" PRIuMAX " output_batch_stderr_bytes=%" PRIuMAX " "
            "worker_local_files_processed=%" PRIuMAX " worker_local_dirs_walked=%" PRIuMAX " worker_subtrees_donated=%" PRIuMAX " worker_donated_subtrees=%" PRIuMAX " worker_stolen_subtrees=%" PRIuMAX " "
            "worker_donated_dir_opened=%" PRIuMAX " worker_donated_dir_open_failures=%" PRIuMAX " worker_donated_dir_owned_walks=%" PRIuMAX " worker_donated_dir_reopen_fallbacks=%" PRIuMAX " "
            "global_queue_pushes=%" PRIuMAX " global_queue_pops=%" PRIuMAX " "
            "ordered_records_submitted=%" PRIuMAX " unordered_flushes=%" PRIuMAX " ",
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dirents_seen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_getdents64_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_getdents64_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dirs_seen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_files_seen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_symlinks_seen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_unknown_dtype_seen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_fstat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_lstat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_fstatat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_unknown_dtype,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_symlink_policy,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_traversal_policy,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_metadata_filter,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_max_filesize,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_min_filesize,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_type,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_sort,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_metadata_output,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_stat_reason_explicit_operand,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_openat_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_join_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_push_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_push_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_pop_calls,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_pop_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_allocs,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_path_copies_before_match,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_literal_basename_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_literal_basename_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_literal_extension_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_literal_extension_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_anchored_prefix_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_anchored_prefix_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_basename_only_fast_paths,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_no_generic_glob_fast_paths,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_builtin_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_builtin_rejects,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_gitignore_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_gitignore_rejects,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_dotignore_checks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_dotignore_rejects,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_glob_fallbacks,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_generic_glob_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_generic_glob_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_git_root_lstat_calls,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_ignore_git_root_lstat_misses,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_filter_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_state_pushes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_state_inline_frames,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_state_fast_paths,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_ignore_state_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_hidden_policy_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_hidden_policy_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_type_policy_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_type_policy_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_cli_glob_checks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_cli_glob_rejects,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_rejected_entries,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.walk_filter_rejected_dirs,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.files_seen, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.dirs_seen, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_submits, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_pops, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_queue_lock_acquires, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_queue_cond_wakeups, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_slot_lock_acquires, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_wakeups, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.path_bytes_copied, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.path_copies_before_match, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batch_files, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batch_path_bytes, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batch_allocs, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batch_storage_reallocs, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batch_lifetime_empty, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_queued, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_empty, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_queued, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.search_batches_empty, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.memstreams_opened, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.output_records_submitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.diagnostic_records_submitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.match_records_submitted, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.ordered_output_records, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.unordered_output_flushes, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.skipped_output_seqs, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_files_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.stolen_files_searched, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_dirs_walked, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.stolen_dirs_walked, memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.queued_output_batches,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.empty_output_batches,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.output_batch_records,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.output_batch_stdout_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.output_batch_stderr_bytes,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_files_searched,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.local_dirs_walked,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_subtrees_donated,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_subtrees_donated,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_subtrees_stolen,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.worker_donated_dir_opened,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.worker_donated_dir_open_failures,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.worker_donated_dir_owned_walks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(
                &current_dev_counters.worker_donated_dir_reopen_fallbacks,
                memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_submits,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.global_pool_pops,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.ordered_output_records,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.unordered_output_flushes,
                                            memory_order_relaxed));

    fprintf(stream,
            "walk_dir_bucket_tiny_dirs=%" PRIuMAX " walk_dir_bucket_tiny_entries=%" PRIuMAX " walk_dir_bucket_tiny_ns=%" PRIuMAX " "
            "walk_dir_bucket_small_dirs=%" PRIuMAX " walk_dir_bucket_small_entries=%" PRIuMAX " walk_dir_bucket_small_ns=%" PRIuMAX " "
            "walk_dir_bucket_medium_dirs=%" PRIuMAX " walk_dir_bucket_medium_entries=%" PRIuMAX " walk_dir_bucket_medium_ns=%" PRIuMAX " "
            "walk_dir_bucket_huge_dirs=%" PRIuMAX " walk_dir_bucket_huge_entries=%" PRIuMAX " walk_dir_bucket_huge_ns=%" PRIuMAX "\n",
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_tiny_dirs,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_tiny_entries,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_tiny_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_small_dirs,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_small_entries,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_small_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_medium_dirs,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_medium_entries,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_medium_ns,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_huge_dirs,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_huge_entries,
                                            memory_order_relaxed),
            (uintmax_t)atomic_load_explicit(&current_dev_counters.walk_dir_bucket_huge_ns,
                                            memory_order_relaxed));
}
