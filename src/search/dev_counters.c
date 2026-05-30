#include <stdbool.h>
#include <stddef.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"

#define BX_SEARCH_DEV_COUNTER_FIELDS(X) \
    X(bytes_read) \
    X(files_opened) \
    X(content_open_calls) \
    X(content_close_calls) \
    X(content_fstat_calls) \
    X(content_fcntl_calls) \
    X(content_read_calls) \
    X(content_read_bytes) \
    X(content_pread_calls) \
    X(content_pread_bytes) \
    X(prefix_pread_calls) \
    X(prefix_pread_bytes) \
    X(prefix_bytes_rescanned) \
    X(transform_prefix_checks) \
    X(binary_prefix_checks) \
    X(files_cut_off_by_binary_prefix) \
    X(candidate_triggered_reopen_calls) \
    X(candidate_triggered_scanner_entries) \
    X(raw_fd_to_scanner_entries) \
    X(raw_fd_to_output_entries) \
    X(raw_fd_to_diagnostic_entries) \
    X(candidate_hits) \
    X(literal_candidate_hits) \
    X(literal_confirm_calls) \
    X(literal_matches) \
    X(literal_not_found) \
    X(literal_overlap_bytes_scanned) \
    X(literal_cross_chunk_matches) \
    X(literal_plan_compiles) \
    X(literal_selected_pair_start) \
    X(literal_selected_pair_interior) \
    X(literal_selected_pair_end) \
    X(literal_algo_empty_calls) \
    X(literal_algo_byte_calls) \
    X(literal_algo_pair_calls) \
    X(literal_algo_short_calls) \
    X(literal_algo_rare_pair_calls) \
    X(literal_algo_long_calls) \
    X(literal_algo_scalar_calls) \
    X(literal_algo_x86_avx2_calls) \
    X(literal_algo_arm64_neon_calls) \
    X(literal_algo_arm64_sve_calls) \
    X(literal_algo_memmem_calls) \
    X(literal_bytes_scanned) \
    X(literal_rare_pair_probe_calls) \
    X(literal_pair_mask_nonzero) \
    X(literal_backend_requested) \
    X(literal_backend_resolved) \
    X(literal_avx2_runtime_available) \
    X(literal_avx2_target_available) \
    X(literal_avx2_eligible_but_not_selected) \
    X(literal_algo_sse2_calls) \
    X(literal_sse2_first_last_calls) \
    X(matcher_invocations) \
    X(records_materialized) \
    X(scanner_entries) \
    X(scanner_entries_from_literal_candidate) \
    X(scanner_entries_without_candidate) \
    X(lines_counted) \
    X(line_boundaries_recovered) \
    X(candidate_triggered_line_recovery_reread_bytes) \
    X(records_expanded) \
    X(plain_line_outputs) \
    X(context_buffer_entries) \
    X(scanner_plain_prefix_allocs) \
    X(output_lines_emitted) \
    X(binary_policy_checks) \
    X(display_path_borrows) \
    X(display_path_copies) \
    X(display_path_copy_bytes) \
    X(walk_dirents_seen) \
    X(walk_getdents64_calls) \
    X(walk_getdents64_bytes) \
    X(walk_dirs_seen) \
    X(walk_files_seen) \
    X(walk_symlinks_seen) \
    X(walk_unknown_dtype_seen) \
    X(walk_stat_calls) \
    X(walk_fstat_calls) \
    X(walk_lstat_calls) \
    X(walk_fstatat_calls) \
    X(walk_stat_reason_unknown_dtype) \
    X(walk_stat_reason_symlink_policy) \
    X(walk_stat_reason_traversal_policy) \
    X(walk_stat_reason_metadata_filter) \
    X(walk_stat_reason_max_filesize) \
    X(walk_stat_reason_min_filesize) \
    X(walk_stat_reason_type) \
    X(walk_stat_reason_sort) \
    X(walk_stat_reason_metadata_output) \
    X(walk_stat_reason_explicit_operand) \
    X(walk_openat_calls) \
    X(walk_path_join_calls) \
    X(walk_path_push_calls) \
    X(walk_path_push_ns) \
    X(walk_path_pop_calls) \
    X(walk_path_pop_ns) \
    X(walk_path_allocs) \
    X(walk_path_copies_before_match) \
    X(walk_dir_bucket_tiny_dirs) \
    X(walk_dir_bucket_tiny_entries) \
    X(walk_dir_bucket_tiny_ns) \
    X(walk_dir_bucket_small_dirs) \
    X(walk_dir_bucket_small_entries) \
    X(walk_dir_bucket_small_ns) \
    X(walk_dir_bucket_medium_dirs) \
    X(walk_dir_bucket_medium_entries) \
    X(walk_dir_bucket_medium_ns) \
    X(walk_dir_bucket_huge_dirs) \
    X(walk_dir_bucket_huge_entries) \
    X(walk_dir_bucket_huge_ns) \
    X(walk_ignore_checks) \
    X(walk_ignore_literal_basename_checks) \
    X(walk_ignore_literal_basename_rejects) \
    X(walk_ignore_literal_extension_checks) \
    X(walk_ignore_literal_extension_rejects) \
    X(walk_ignore_anchored_prefix_checks) \
    X(walk_ignore_anchored_prefix_rejects) \
    X(walk_ignore_basename_only_fast_paths) \
    X(walk_ignore_no_generic_glob_fast_paths) \
    X(walk_ignore_builtin_checks) \
    X(walk_ignore_builtin_rejects) \
    X(walk_ignore_gitignore_checks) \
    X(walk_ignore_gitignore_rejects) \
    X(walk_ignore_dotignore_checks) \
    X(walk_ignore_dotignore_rejects) \
    X(walk_ignore_glob_fallbacks) \
    X(walk_ignore_generic_glob_checks) \
    X(walk_ignore_generic_glob_rejects) \
    X(walk_ignore_git_root_lstat_calls) \
    X(walk_ignore_git_root_lstat_misses) \
    X(walk_filter_ns) \
    X(walk_ignore_state_pushes) \
    X(walk_ignore_state_inline_frames) \
    X(walk_ignore_state_fast_paths) \
    X(walk_ignore_state_ns) \
    X(walk_filter_hidden_policy_checks) \
    X(walk_filter_hidden_policy_rejects) \
    X(walk_filter_type_policy_checks) \
    X(walk_filter_type_policy_rejects) \
    X(walk_filter_cli_glob_checks) \
    X(walk_filter_cli_glob_rejects) \
    X(walk_filter_rejected_entries) \
    X(walk_filter_rejected_dirs) \
    X(files_seen) \
    X(dirs_seen) \
    X(global_pool_submits) \
    X(global_pool_pops) \
    X(global_queue_lock_acquires) \
    X(global_queue_cond_wakeups) \
    X(worker_slot_lock_acquires) \
    X(worker_wakeups) \
    X(path_bytes_copied) \
    X(path_copies_before_match) \
    X(search_batch_files) \
    X(search_batch_path_bytes) \
    X(search_batch_allocs) \
    X(search_batch_storage_reallocs) \
    X(search_batch_lifetime_empty) \
    X(search_batches_queued) \
    X(search_batches_searched) \
    X(search_batches_empty) \
    X(memstreams_opened) \
    X(output_records_submitted) \
    X(diagnostic_records_submitted) \
    X(match_records_submitted) \
    X(ordered_output_records) \
    X(unordered_output_flushes) \
    X(skipped_output_seqs) \
    X(local_files_searched) \
    X(stolen_files_searched) \
    X(local_dirs_walked) \
    X(stolen_dirs_walked) \
    X(queued_output_batches) \
    X(empty_output_batches) \
    X(output_batch_records) \
    X(output_batch_stdout_bytes) \
    X(output_batch_stderr_bytes) \
    X(worker_subtrees_donated) \
    X(worker_subtrees_stolen) \
    X(worker_donated_dir_opened) \
    X(worker_donated_dir_open_failures) \
    X(worker_donated_dir_owned_walks) \
    X(worker_donated_dir_reopen_fallbacks)

#define BX_SEARCH_DEV_COUNTER_SUM_FIELDS(X) \
    X(bytes_read) \
    X(files_opened) \
    X(content_open_calls) \
    X(content_close_calls) \
    X(content_fstat_calls) \
    X(content_fcntl_calls) \
    X(content_read_calls) \
    X(content_read_bytes) \
    X(content_pread_calls) \
    X(content_pread_bytes) \
    X(prefix_pread_calls) \
    X(prefix_pread_bytes) \
    X(prefix_bytes_rescanned) \
    X(transform_prefix_checks) \
    X(binary_prefix_checks) \
    X(files_cut_off_by_binary_prefix) \
    X(candidate_triggered_reopen_calls) \
    X(candidate_triggered_scanner_entries) \
    X(raw_fd_to_scanner_entries) \
    X(raw_fd_to_output_entries) \
    X(raw_fd_to_diagnostic_entries) \
    X(candidate_hits) \
    X(literal_candidate_hits) \
    X(literal_confirm_calls) \
    X(literal_matches) \
    X(literal_not_found) \
    X(literal_overlap_bytes_scanned) \
    X(literal_cross_chunk_matches) \
    X(literal_plan_compiles) \
    X(literal_selected_pair_start) \
    X(literal_selected_pair_interior) \
    X(literal_selected_pair_end) \
    X(literal_algo_empty_calls) \
    X(literal_algo_byte_calls) \
    X(literal_algo_pair_calls) \
    X(literal_algo_short_calls) \
    X(literal_algo_rare_pair_calls) \
    X(literal_algo_long_calls) \
    X(literal_algo_scalar_calls) \
    X(literal_algo_x86_avx2_calls) \
    X(literal_algo_arm64_neon_calls) \
    X(literal_algo_arm64_sve_calls) \
    X(literal_algo_memmem_calls) \
    X(literal_bytes_scanned) \
    X(literal_rare_pair_probe_calls) \
    X(literal_pair_mask_nonzero) \
    X(literal_avx2_eligible_but_not_selected) \
    X(literal_algo_sse2_calls) \
    X(literal_sse2_first_last_calls) \
    X(matcher_invocations) \
    X(records_materialized) \
    X(scanner_entries) \
    X(scanner_entries_from_literal_candidate) \
    X(scanner_entries_without_candidate) \
    X(lines_counted) \
    X(line_boundaries_recovered) \
    X(candidate_triggered_line_recovery_reread_bytes) \
    X(records_expanded) \
    X(plain_line_outputs) \
    X(context_buffer_entries) \
    X(scanner_plain_prefix_allocs) \
    X(output_lines_emitted) \
    X(binary_policy_checks) \
    X(display_path_borrows) \
    X(display_path_copies) \
    X(display_path_copy_bytes) \
    X(walk_dirents_seen) \
    X(walk_getdents64_calls) \
    X(walk_getdents64_bytes) \
    X(walk_dirs_seen) \
    X(walk_files_seen) \
    X(walk_symlinks_seen) \
    X(walk_unknown_dtype_seen) \
    X(walk_stat_calls) \
    X(walk_fstat_calls) \
    X(walk_lstat_calls) \
    X(walk_fstatat_calls) \
    X(walk_stat_reason_unknown_dtype) \
    X(walk_stat_reason_symlink_policy) \
    X(walk_stat_reason_traversal_policy) \
    X(walk_stat_reason_metadata_filter) \
    X(walk_stat_reason_max_filesize) \
    X(walk_stat_reason_min_filesize) \
    X(walk_stat_reason_type) \
    X(walk_stat_reason_sort) \
    X(walk_stat_reason_metadata_output) \
    X(walk_stat_reason_explicit_operand) \
    X(walk_openat_calls) \
    X(walk_path_join_calls) \
    X(walk_path_push_calls) \
    X(walk_path_push_ns) \
    X(walk_path_pop_calls) \
    X(walk_path_pop_ns) \
    X(walk_path_allocs) \
    X(walk_path_copies_before_match) \
    X(walk_dir_bucket_tiny_dirs) \
    X(walk_dir_bucket_tiny_entries) \
    X(walk_dir_bucket_tiny_ns) \
    X(walk_dir_bucket_small_dirs) \
    X(walk_dir_bucket_small_entries) \
    X(walk_dir_bucket_small_ns) \
    X(walk_dir_bucket_medium_dirs) \
    X(walk_dir_bucket_medium_entries) \
    X(walk_dir_bucket_medium_ns) \
    X(walk_dir_bucket_huge_dirs) \
    X(walk_dir_bucket_huge_entries) \
    X(walk_dir_bucket_huge_ns) \
    X(walk_ignore_checks) \
    X(walk_ignore_literal_basename_checks) \
    X(walk_ignore_literal_basename_rejects) \
    X(walk_ignore_literal_extension_checks) \
    X(walk_ignore_literal_extension_rejects) \
    X(walk_ignore_anchored_prefix_checks) \
    X(walk_ignore_anchored_prefix_rejects) \
    X(walk_ignore_basename_only_fast_paths) \
    X(walk_ignore_no_generic_glob_fast_paths) \
    X(walk_ignore_builtin_checks) \
    X(walk_ignore_builtin_rejects) \
    X(walk_ignore_gitignore_checks) \
    X(walk_ignore_gitignore_rejects) \
    X(walk_ignore_dotignore_checks) \
    X(walk_ignore_dotignore_rejects) \
    X(walk_ignore_glob_fallbacks) \
    X(walk_ignore_generic_glob_checks) \
    X(walk_ignore_generic_glob_rejects) \
    X(walk_ignore_git_root_lstat_calls) \
    X(walk_ignore_git_root_lstat_misses) \
    X(walk_filter_ns) \
    X(walk_ignore_state_pushes) \
    X(walk_ignore_state_inline_frames) \
    X(walk_ignore_state_fast_paths) \
    X(walk_ignore_state_ns) \
    X(walk_filter_hidden_policy_checks) \
    X(walk_filter_hidden_policy_rejects) \
    X(walk_filter_type_policy_checks) \
    X(walk_filter_type_policy_rejects) \
    X(walk_filter_cli_glob_checks) \
    X(walk_filter_cli_glob_rejects) \
    X(walk_filter_rejected_entries) \
    X(walk_filter_rejected_dirs) \
    X(files_seen) \
    X(dirs_seen) \
    X(global_pool_submits) \
    X(global_pool_pops) \
    X(global_queue_lock_acquires) \
    X(global_queue_cond_wakeups) \
    X(worker_slot_lock_acquires) \
    X(worker_wakeups) \
    X(path_bytes_copied) \
    X(path_copies_before_match) \
    X(search_batch_files) \
    X(search_batch_path_bytes) \
    X(search_batch_allocs) \
    X(search_batch_storage_reallocs) \
    X(search_batch_lifetime_empty) \
    X(search_batches_queued) \
    X(search_batches_searched) \
    X(search_batches_empty) \
    X(memstreams_opened) \
    X(output_records_submitted) \
    X(diagnostic_records_submitted) \
    X(match_records_submitted) \
    X(ordered_output_records) \
    X(unordered_output_flushes) \
    X(skipped_output_seqs) \
    X(local_files_searched) \
    X(stolen_files_searched) \
    X(local_dirs_walked) \
    X(stolen_dirs_walked) \
    X(queued_output_batches) \
    X(empty_output_batches) \
    X(output_batch_records) \
    X(output_batch_stdout_bytes) \
    X(output_batch_stderr_bytes) \
    X(worker_subtrees_donated) \
    X(worker_subtrees_stolen) \
    X(worker_donated_dir_opened) \
    X(worker_donated_dir_open_failures) \
    X(worker_donated_dir_owned_walks) \
    X(worker_donated_dir_reopen_fallbacks)

#define BX_SEARCH_DEV_COUNTER_GAUGE_FIELDS(X) \
    X(literal_backend_requested) \
    X(literal_backend_resolved) \
    X(literal_avx2_runtime_available) \
    X(literal_avx2_target_available)

struct bx_search_dev_counter_values {
    /*
     * Selected-pair distribution probes are sharded with the rest of the
     * counter state now; historical audit anchors kept here:
     * atomic_size_t literal_selected_pair_start;
     * atomic_size_t literal_selected_pair_interior;
     * atomic_size_t literal_selected_pair_end;
     */
    size_t bytes_read;
    size_t files_opened;
    uint_fast64_t content_open_calls;
    uint_fast64_t content_close_calls;
    uint_fast64_t content_fstat_calls;
    uint_fast64_t content_fcntl_calls;
    uint_fast64_t content_read_calls;
    uint_fast64_t content_read_bytes;
    uint_fast64_t content_pread_calls;
    uint_fast64_t content_pread_bytes;
    uint_fast64_t prefix_pread_calls;
    uint_fast64_t prefix_pread_bytes;
    uint_fast64_t prefix_bytes_rescanned;
    uint_fast64_t transform_prefix_checks;
    uint_fast64_t binary_prefix_checks;
    uint_fast64_t files_cut_off_by_binary_prefix;
    uint_fast64_t candidate_triggered_reopen_calls;
    uint_fast64_t candidate_triggered_scanner_entries;
    uint_fast64_t raw_fd_to_scanner_entries;
    uint_fast64_t raw_fd_to_output_entries;
    uint_fast64_t raw_fd_to_diagnostic_entries;
    size_t candidate_hits;
    size_t literal_candidate_hits;
    size_t literal_confirm_calls;
    size_t literal_matches;
    size_t literal_not_found;
    size_t literal_overlap_bytes_scanned;
    size_t literal_cross_chunk_matches;
    size_t literal_plan_compiles;
    size_t literal_selected_pair_start;
    size_t literal_selected_pair_interior;
    size_t literal_selected_pair_end;
    size_t literal_algo_empty_calls;
    size_t literal_algo_byte_calls;
    size_t literal_algo_pair_calls;
    size_t literal_algo_short_calls;
    size_t literal_algo_rare_pair_calls;
    size_t literal_algo_long_calls;
    size_t literal_algo_scalar_calls;
    size_t literal_algo_x86_avx2_calls;
    size_t literal_algo_arm64_neon_calls;
    size_t literal_algo_arm64_sve_calls;
    size_t literal_algo_memmem_calls;
    size_t literal_bytes_scanned;
    size_t literal_rare_pair_probe_calls;
    size_t literal_pair_mask_nonzero;
    uint_fast64_t literal_backend_requested;
    uint_fast64_t literal_backend_resolved;
    uint_fast64_t literal_avx2_runtime_available;
    uint_fast64_t literal_avx2_target_available;
    uint_fast64_t literal_avx2_eligible_but_not_selected;
    size_t literal_algo_sse2_calls;
    size_t literal_sse2_first_last_calls;
    size_t matcher_invocations;
    size_t records_materialized;
    size_t scanner_entries;
    size_t scanner_entries_from_literal_candidate;
    size_t scanner_entries_without_candidate;
    size_t lines_counted;
    size_t line_boundaries_recovered;
    size_t candidate_triggered_line_recovery_reread_bytes;
    size_t records_expanded;
    size_t plain_line_outputs;
    size_t context_buffer_entries;
    size_t scanner_plain_prefix_allocs;
    size_t output_lines_emitted;
    uint_fast64_t binary_policy_checks;
    uint_fast64_t display_path_borrows;
    uint_fast64_t display_path_copies;
    uint_fast64_t display_path_copy_bytes;
    uint_fast64_t walk_dirents_seen;
    uint_fast64_t walk_getdents64_calls;
    uint_fast64_t walk_getdents64_bytes;
    uint_fast64_t walk_dirs_seen;
    uint_fast64_t walk_files_seen;
    uint_fast64_t walk_symlinks_seen;
    uint_fast64_t walk_unknown_dtype_seen;
    uint_fast64_t walk_stat_calls;
    uint_fast64_t walk_fstat_calls;
    uint_fast64_t walk_lstat_calls;
    uint_fast64_t walk_fstatat_calls;
    uint_fast64_t walk_stat_reason_unknown_dtype;
    uint_fast64_t walk_stat_reason_symlink_policy;
    uint_fast64_t walk_stat_reason_traversal_policy;
    uint_fast64_t walk_stat_reason_metadata_filter;
    uint_fast64_t walk_stat_reason_max_filesize;
    uint_fast64_t walk_stat_reason_min_filesize;
    uint_fast64_t walk_stat_reason_type;
    uint_fast64_t walk_stat_reason_sort;
    uint_fast64_t walk_stat_reason_metadata_output;
    uint_fast64_t walk_stat_reason_explicit_operand;
    uint_fast64_t walk_openat_calls;
    uint_fast64_t walk_path_join_calls;
    uint_fast64_t walk_path_push_calls;
    uint_fast64_t walk_path_push_ns;
    uint_fast64_t walk_path_pop_calls;
    uint_fast64_t walk_path_pop_ns;
    uint_fast64_t walk_path_allocs;
    uint_fast64_t walk_path_copies_before_match;
    uint_fast64_t walk_dir_bucket_tiny_dirs;
    uint_fast64_t walk_dir_bucket_tiny_entries;
    uint_fast64_t walk_dir_bucket_tiny_ns;
    uint_fast64_t walk_dir_bucket_small_dirs;
    uint_fast64_t walk_dir_bucket_small_entries;
    uint_fast64_t walk_dir_bucket_small_ns;
    uint_fast64_t walk_dir_bucket_medium_dirs;
    uint_fast64_t walk_dir_bucket_medium_entries;
    uint_fast64_t walk_dir_bucket_medium_ns;
    uint_fast64_t walk_dir_bucket_huge_dirs;
    uint_fast64_t walk_dir_bucket_huge_entries;
    uint_fast64_t walk_dir_bucket_huge_ns;
    uint_fast64_t walk_ignore_checks;
    uint_fast64_t walk_ignore_literal_basename_checks;
    uint_fast64_t walk_ignore_literal_basename_rejects;
    uint_fast64_t walk_ignore_literal_extension_checks;
    uint_fast64_t walk_ignore_literal_extension_rejects;
    uint_fast64_t walk_ignore_anchored_prefix_checks;
    uint_fast64_t walk_ignore_anchored_prefix_rejects;
    uint_fast64_t walk_ignore_basename_only_fast_paths;
    uint_fast64_t walk_ignore_no_generic_glob_fast_paths;
    uint_fast64_t walk_ignore_builtin_checks;
    uint_fast64_t walk_ignore_builtin_rejects;
    uint_fast64_t walk_ignore_gitignore_checks;
    uint_fast64_t walk_ignore_gitignore_rejects;
    uint_fast64_t walk_ignore_dotignore_checks;
    uint_fast64_t walk_ignore_dotignore_rejects;
    uint_fast64_t walk_ignore_glob_fallbacks;
    uint_fast64_t walk_ignore_generic_glob_checks;
    uint_fast64_t walk_ignore_generic_glob_rejects;
    uint_fast64_t walk_ignore_git_root_lstat_calls;
    uint_fast64_t walk_ignore_git_root_lstat_misses;
    uint_fast64_t walk_filter_ns;
    uint_fast64_t walk_ignore_state_pushes;
    uint_fast64_t walk_ignore_state_inline_frames;
    uint_fast64_t walk_ignore_state_fast_paths;
    uint_fast64_t walk_ignore_state_ns;
    uint_fast64_t walk_filter_hidden_policy_checks;
    uint_fast64_t walk_filter_hidden_policy_rejects;
    uint_fast64_t walk_filter_type_policy_checks;
    uint_fast64_t walk_filter_type_policy_rejects;
    uint_fast64_t walk_filter_cli_glob_checks;
    uint_fast64_t walk_filter_cli_glob_rejects;
    uint_fast64_t walk_filter_rejected_entries;
    uint_fast64_t walk_filter_rejected_dirs;
    uint_fast64_t files_seen;
    uint_fast64_t dirs_seen;
    uint_fast64_t global_pool_submits;
    uint_fast64_t global_pool_pops;
    uint_fast64_t global_queue_lock_acquires;
    uint_fast64_t global_queue_cond_wakeups;
    uint_fast64_t worker_slot_lock_acquires;
    uint_fast64_t worker_wakeups;
    uint_fast64_t path_bytes_copied;
    uint_fast64_t path_copies_before_match;
    uint_fast64_t search_batch_files;
    uint_fast64_t search_batch_path_bytes;
    uint_fast64_t search_batch_allocs;
    uint_fast64_t search_batch_storage_reallocs;
    uint_fast64_t search_batch_lifetime_empty;
    uint_fast64_t search_batches_queued;
    uint_fast64_t search_batches_searched;
    uint_fast64_t search_batches_empty;
    uint_fast64_t memstreams_opened;
    uint_fast64_t output_records_submitted;
    uint_fast64_t diagnostic_records_submitted;
    uint_fast64_t match_records_submitted;
    uint_fast64_t ordered_output_records;
    uint_fast64_t unordered_output_flushes;
    uint_fast64_t skipped_output_seqs;
    uint_fast64_t local_files_searched;
    uint_fast64_t stolen_files_searched;
    uint_fast64_t local_dirs_walked;
    uint_fast64_t stolen_dirs_walked;
    uint_fast64_t queued_output_batches;
    uint_fast64_t empty_output_batches;
    uint_fast64_t output_batch_records;
    uint_fast64_t output_batch_stdout_bytes;
    uint_fast64_t output_batch_stderr_bytes;
    uint_fast64_t worker_subtrees_donated;
    uint_fast64_t worker_subtrees_stolen;
    uint_fast64_t worker_donated_dir_opened;
    uint_fast64_t worker_donated_dir_open_failures;
    uint_fast64_t worker_donated_dir_owned_walks;
    uint_fast64_t worker_donated_dir_reopen_fallbacks;
};

struct bx_search_dev_counter_shard {
    struct bx_search_dev_counter_values values;
    uint64_t generation;
    struct bx_search_dev_counter_shard *next;
};

struct bx_search_dev_counters {
    bool enabled;
    bool batch_debug_enabled;
    uint64_t batch_debug_next_id;
    uint64_t generation;
    pthread_mutex_t lock;
    struct bx_search_dev_counter_shard *shards;
};

static struct bx_search_dev_counters current_dev_counters = {
    .batch_debug_next_id = 1u,
    .generation = 1u,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

static _Thread_local struct bx_search_dev_counter_shard *thread_dev_counter_shard;
static _Thread_local uint64_t thread_dev_counter_generation;

bool bx_search_dev_counters_enabled(void) {
    return current_dev_counters.enabled;
}

static struct bx_search_dev_counter_values *bx_search_dev_counters_thread_values(void) {
    struct bx_search_dev_counter_shard *shard;

    if (thread_dev_counter_generation == current_dev_counters.generation &&
        thread_dev_counter_shard != NULL) {
        return &thread_dev_counter_shard->values;
    }

    shard = calloc(1u, sizeof(*shard));
    if (shard == NULL)
        return NULL;

    shard->generation = current_dev_counters.generation;
    pthread_mutex_lock(&current_dev_counters.lock);
    shard->next = current_dev_counters.shards;
    current_dev_counters.shards = shard;
    pthread_mutex_unlock(&current_dev_counters.lock);

    thread_dev_counter_shard = shard;
    thread_dev_counter_generation = shard->generation;
    return &shard->values;
}

#define BX_SEARCH_DEV_COUNTER_ADD(field, amount)                                \
    do {                                                                        \
        struct bx_search_dev_counter_values *values__ =                         \
            bx_search_dev_counters_thread_values();                             \
        if (values__ != NULL)                                                   \
            values__->field += (amount);                                        \
    } while (0)

#define BX_SEARCH_DEV_COUNTER_SET(field, value)                                 \
    do {                                                                        \
        struct bx_search_dev_counter_values *values__ =                         \
            bx_search_dev_counters_thread_values();                             \
        if (values__ != NULL)                                                   \
            values__->field = (value);                                          \
    } while (0)

static void bx_search_dev_counter_shards_free(
    struct bx_search_dev_counter_shard *shard) {
    while (shard != NULL) {
        struct bx_search_dev_counter_shard *next = shard->next;
        free(shard);
        shard = next;
    }
}

static void bx_search_dev_counters_reduce(
    struct bx_search_dev_counter_values *out) {
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&current_dev_counters.lock);
    for (struct bx_search_dev_counter_shard *shard = current_dev_counters.shards;
         shard != NULL; shard = shard->next) {
#define BX_REDUCE_SUM(field) out->field += shard->values.field;
        BX_SEARCH_DEV_COUNTER_SUM_FIELDS(BX_REDUCE_SUM)
#undef BX_REDUCE_SUM
#define BX_REDUCE_GAUGE(field)                                                  \
        do {                                                                    \
            if (shard->values.field > out->field)                               \
                out->field = shard->values.field;                               \
        } while (0);
        BX_SEARCH_DEV_COUNTER_GAUGE_FIELDS(BX_REDUCE_GAUGE)
#undef BX_REDUCE_GAUGE
    }
    pthread_mutex_unlock(&current_dev_counters.lock);
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
    struct bx_search_dev_counter_shard *old_shards;

    current_dev_counters.enabled = false;
    current_dev_counters.batch_debug_enabled = false;

    pthread_mutex_lock(&current_dev_counters.lock);
    current_dev_counters.batch_debug_next_id = 1u;
    current_dev_counters.generation++;
    old_shards = current_dev_counters.shards;
    current_dev_counters.shards = NULL;
    pthread_mutex_unlock(&current_dev_counters.lock);

    bx_search_dev_counter_shards_free(old_shards);
    thread_dev_counter_shard = NULL;
    thread_dev_counter_generation = current_dev_counters.generation;
}

void bx_search_dev_counters_note_bytes_read(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(bytes_read, count);
}

void bx_search_dev_counters_note_file_opened(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(files_opened, 1u);
}

void bx_search_dev_counters_note_content_open_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_open_calls, 1u);
}

void bx_search_dev_counters_note_content_close_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_close_calls, 1u);
}

void bx_search_dev_counters_note_content_fstat_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_fstat_calls, 1u);
}

void bx_search_dev_counters_note_content_fcntl_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_fcntl_calls, 1u);
}

void bx_search_dev_counters_note_content_read(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_read_calls, 1u);
    if (count == 0u)
        return;
    BX_SEARCH_DEV_COUNTER_ADD(content_read_bytes, count);
    BX_SEARCH_DEV_COUNTER_ADD(bytes_read, count);
}

void bx_search_dev_counters_note_content_pread(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(content_pread_calls, 1u);
    if (count == 0u)
        return;
    BX_SEARCH_DEV_COUNTER_ADD(content_pread_bytes, count);
}

void bx_search_dev_counters_note_prefix_pread(size_t count) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(prefix_pread_calls, 1u);
    if (count == 0u)
        return;
    BX_SEARCH_DEV_COUNTER_ADD(prefix_pread_bytes, count);
}

void bx_search_dev_counters_note_prefix_bytes_rescanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(prefix_bytes_rescanned, count);
}

void bx_search_dev_counters_note_transform_prefix_check(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(transform_prefix_checks, 1u);
}

void bx_search_dev_counters_note_binary_prefix_check(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(binary_prefix_checks, 1u);
}

void bx_search_dev_counters_note_file_cut_off_by_binary_prefix(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(files_cut_off_by_binary_prefix, 1u);
}

void bx_search_dev_counters_note_candidate_triggered_reopen_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(candidate_triggered_reopen_calls, 1u);
}

void bx_search_dev_counters_note_candidate_triggered_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(candidate_triggered_scanner_entries, 1u);
}

void bx_search_dev_counters_note_raw_fd_to_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(raw_fd_to_scanner_entries, 1u);
}

void bx_search_dev_counters_note_raw_fd_to_output_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(raw_fd_to_output_entries, 1u);
}

void bx_search_dev_counters_note_raw_fd_to_diagnostic_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(raw_fd_to_diagnostic_entries, 1u);
}

void bx_search_dev_counters_note_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(candidate_hits, 1u);
}

void bx_search_dev_counters_note_literal_candidate_hit(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(candidate_hits, 1u);
    BX_SEARCH_DEV_COUNTER_ADD(literal_candidate_hits, 1u);
}

void bx_search_dev_counters_note_literal_confirm_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_confirm_calls, 1u);
}

void bx_search_dev_counters_note_literal_match(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_matches, 1u);
}

void bx_search_dev_counters_note_literal_not_found(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_not_found, 1u);
}

void bx_search_dev_counters_note_literal_overlap_bytes_scanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_overlap_bytes_scanned, count);
}

void bx_search_dev_counters_note_literal_cross_chunk_match(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_cross_chunk_matches, 1u);
}

void bx_search_dev_counters_note_literal_plan_compile(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_plan_compiles, 1u);
}

void bx_search_dev_counters_note_literal_selected_pair_distribution(size_t pair_offset,
                                                                    size_t needle_len) {
    if (!current_dev_counters.enabled || needle_len < 2u)
        return;

    if (pair_offset == 0u) {
        BX_SEARCH_DEV_COUNTER_ADD(literal_selected_pair_start, 1u);
        return;
    }
    if (pair_offset + 2u == needle_len) {
        BX_SEARCH_DEV_COUNTER_ADD(literal_selected_pair_end, 1u);
        return;
    }
    BX_SEARCH_DEV_COUNTER_ADD(literal_selected_pair_interior, 1u);
}

void bx_search_dev_counters_note_literal_algo_empty_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_empty_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_byte_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_byte_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_pair_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_pair_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_short_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_short_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_rare_pair_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_rare_pair_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_long_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_long_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_scalar_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_scalar_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_x86_avx2_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_x86_avx2_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_arm64_neon_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_arm64_neon_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_arm64_sve_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_arm64_sve_calls, 1u);
}

void bx_search_dev_counters_note_literal_algo_memmem_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_memmem_calls, 1u);
}

void bx_search_dev_counters_note_literal_bytes_scanned(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_bytes_scanned, count);
}

void bx_search_dev_counters_note_literal_rare_pair_probe_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_rare_pair_probe_calls, 1u);
}

void bx_search_dev_counters_note_literal_pair_mask_nonzero(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_pair_mask_nonzero, 1u);
}

void bx_search_dev_counters_note_literal_backend_selection(
    uint64_t requested,
    uint64_t resolved,
    bool avx2_runtime_available,
    bool avx2_target_available,
    bool avx2_eligible_but_not_selected) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_SET(literal_backend_requested, requested);
    BX_SEARCH_DEV_COUNTER_SET(literal_backend_resolved, resolved);
    BX_SEARCH_DEV_COUNTER_SET(literal_avx2_runtime_available, avx2_runtime_available ? 1u : 0u);
    BX_SEARCH_DEV_COUNTER_SET(literal_avx2_target_available, avx2_target_available ? 1u : 0u);
    if (avx2_eligible_but_not_selected) {
        BX_SEARCH_DEV_COUNTER_ADD(literal_avx2_eligible_but_not_selected, 1u);
    }
}

void bx_search_dev_counters_note_literal_algo_sse2_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_algo_sse2_calls, 1u);
}

void bx_search_dev_counters_note_literal_sse2_first_last_call(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(literal_sse2_first_last_calls, 1u);
}

void bx_search_dev_counters_note_matcher_invocation(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(matcher_invocations, 1u);
}

void bx_search_dev_counters_note_record_materialized(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(records_materialized, 1u);
}

void bx_search_dev_counters_note_scanner_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(scanner_entries, 1u);
}

void bx_search_dev_counters_note_scanner_entry_from_literal_candidate(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(scanner_entries_from_literal_candidate, 1u);
}

void bx_search_dev_counters_note_scanner_entry_without_candidate(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(scanner_entries_without_candidate, 1u);
}

void bx_search_dev_counters_note_lines_counted(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(lines_counted, count);
}

void bx_search_dev_counters_note_line_boundaries_recovered(size_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(line_boundaries_recovered, count);
}

void bx_search_dev_counters_note_candidate_triggered_line_recovery_reread(size_t bytes) {
    if (!current_dev_counters.enabled || bytes == 0u)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(candidate_triggered_line_recovery_reread_bytes, bytes);
}

void bx_search_dev_counters_note_record_expanded(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(records_expanded, 1u);
}

void bx_search_dev_counters_note_plain_line_output(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(plain_line_outputs, 1u);
}

void bx_search_dev_counters_note_context_buffer_entry(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(context_buffer_entries, 1u);
}

void bx_search_dev_counters_note_scanner_plain_prefix_alloc(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(scanner_plain_prefix_allocs, 1u);
}

void bx_search_dev_counters_note_output_line_emitted(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(output_lines_emitted, 1u);
}

void bx_search_dev_counters_note_binary_policy_check(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(binary_policy_checks, 1u);
}

void bx_search_dev_counters_note_display_path_borrow(void) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(display_path_borrows, 1u);
}

void bx_search_dev_counters_note_display_path_copy(size_t bytes) {
    if (!current_dev_counters.enabled)
        return;

    BX_SEARCH_DEV_COUNTER_ADD(display_path_copies, 1u);
    BX_SEARCH_DEV_COUNTER_ADD(display_path_copy_bytes, bytes);
}

void bx_search_dev_counters_note_walk(enum bx_search_walk_counter counter,
                                      uint64_t count) {
    if (!current_dev_counters.enabled || count == 0u)
        return;

    switch (counter) {
    case BX_SEARCH_WALK_DIRENTS_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dirents_seen, count);
        return;
    case BX_SEARCH_WALK_GETDENTS64_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_getdents64_calls, count);
        return;
    case BX_SEARCH_WALK_GETDENTS64_BYTES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_getdents64_bytes, count);
        return;
    case BX_SEARCH_WALK_DIRS_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dirs_seen, count);
        return;
    case BX_SEARCH_WALK_FILES_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(walk_files_seen, count);
        return;
    case BX_SEARCH_WALK_SYMLINKS_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(walk_symlinks_seen, count);
        return;
    case BX_SEARCH_WALK_UNKNOWN_DTYPE_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(walk_unknown_dtype_seen, count);
        return;
    case BX_SEARCH_WALK_STAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_calls, count);
        return;
    case BX_SEARCH_WALK_FSTAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_fstat_calls, count);
        return;
    case BX_SEARCH_WALK_LSTAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_lstat_calls, count);
        return;
    case BX_SEARCH_WALK_FSTATAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_fstatat_calls, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_UNKNOWN_DTYPE:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_unknown_dtype, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_SYMLINK_POLICY:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_symlink_policy, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_TRAVERSAL_POLICY:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_traversal_policy, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_METADATA_FILTER:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_metadata_filter, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_MAX_FILESIZE:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_max_filesize, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_MIN_FILESIZE:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_min_filesize, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_TYPE:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_type, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_SORT:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_sort, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_METADATA_OUTPUT:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_metadata_output, count);
        return;
    case BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND:
        BX_SEARCH_DEV_COUNTER_ADD(walk_stat_reason_explicit_operand, count);
        return;
    case BX_SEARCH_WALK_OPENAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_openat_calls, count);
        return;
    case BX_SEARCH_WALK_PATH_JOIN_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_join_calls, count);
        return;
    case BX_SEARCH_WALK_PATH_PUSH_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_push_calls, count);
        return;
    case BX_SEARCH_WALK_PATH_PUSH_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_push_ns, count);
        return;
    case BX_SEARCH_WALK_PATH_POP_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_pop_calls, count);
        return;
    case BX_SEARCH_WALK_PATH_POP_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_pop_ns, count);
        return;
    case BX_SEARCH_WALK_PATH_ALLOCS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_allocs, count);
        return;
    case BX_SEARCH_WALK_PATH_COPIES_BEFORE_MATCH:
        BX_SEARCH_DEV_COUNTER_ADD(walk_path_copies_before_match, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_DIRS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_tiny_dirs, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_ENTRIES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_tiny_entries, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_TINY_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_tiny_ns, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_DIRS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_small_dirs, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_ENTRIES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_small_entries, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_SMALL_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_small_ns, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_DIRS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_medium_dirs, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_ENTRIES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_medium_entries, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_MEDIUM_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_medium_ns, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_DIRS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_huge_dirs, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_ENTRIES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_huge_entries, count);
        return;
    case BX_SEARCH_WALK_DIR_BUCKET_HUGE_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_dir_bucket_huge_ns, count);
        return;
    case BX_SEARCH_WALK_IGNORE_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_literal_basename_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_literal_basename_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_literal_extension_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_literal_extension_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_anchored_prefix_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_anchored_prefix_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_BASENAME_ONLY_FAST_PATHS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_basename_only_fast_paths, count);
        return;
    case BX_SEARCH_WALK_IGNORE_NO_GENERIC_GLOB_FAST_PATHS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_no_generic_glob_fast_paths, count);
        return;
    case BX_SEARCH_WALK_IGNORE_BUILTIN_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_builtin_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_BUILTIN_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_builtin_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GITIGNORE_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_gitignore_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GITIGNORE_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_gitignore_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_DOTIGNORE_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_dotignore_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_DOTIGNORE_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_dotignore_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_glob_fallbacks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_generic_glob_checks, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_generic_glob_rejects, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_CALLS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_git_root_lstat_calls, count);
        return;
    case BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_MISSES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_git_root_lstat_misses, count);
        return;
    case BX_SEARCH_WALK_FILTER_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_ns, count);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_PUSHES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_state_pushes, count);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_INLINE_FRAMES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_state_inline_frames, count);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_FAST_PATHS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_state_fast_paths, count);
        return;
    case BX_SEARCH_WALK_IGNORE_STATE_NS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_ignore_state_ns, count);
        return;
    case BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_hidden_policy_checks, count);
        return;
    case BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_hidden_policy_rejects, count);
        return;
    case BX_SEARCH_WALK_FILTER_TYPE_POLICY_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_type_policy_checks, count);
        return;
    case BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_type_policy_rejects, count);
        return;
    case BX_SEARCH_WALK_FILTER_CLI_GLOB_CHECKS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_cli_glob_checks, count);
        return;
    case BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_cli_glob_rejects, count);
        return;
    case BX_SEARCH_WALK_FILTER_REJECTED_ENTRIES:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_rejected_entries, count);
        return;
    case BX_SEARCH_WALK_FILTER_REJECTED_DIRS:
        BX_SEARCH_DEV_COUNTER_ADD(walk_filter_rejected_dirs, count);
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
        BX_SEARCH_DEV_COUNTER_ADD(files_seen, count);
        return;
    case BX_SEARCH_RG_SCHED_DIRS_SEEN:
        BX_SEARCH_DEV_COUNTER_ADD(dirs_seen, count);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_POOL_SUBMITS:
        BX_SEARCH_DEV_COUNTER_ADD(global_pool_submits, count);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_POOL_POPS:
        BX_SEARCH_DEV_COUNTER_ADD(global_pool_pops, count);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_LOCK_ACQUIRES:
        BX_SEARCH_DEV_COUNTER_ADD(global_queue_lock_acquires, count);
        return;
    case BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_COND_WAKEUPS:
        BX_SEARCH_DEV_COUNTER_ADD(global_queue_cond_wakeups, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SLOT_LOCK_ACQUIRES:
        BX_SEARCH_DEV_COUNTER_ADD(worker_slot_lock_acquires, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_WAKEUPS:
        BX_SEARCH_DEV_COUNTER_ADD(worker_wakeups, count);
        return;
    case BX_SEARCH_RG_SCHED_PATH_BYTES_COPIED:
        BX_SEARCH_DEV_COUNTER_ADD(path_bytes_copied, count);
        return;
    case BX_SEARCH_RG_SCHED_PATH_COPIES_BEFORE_MATCH:
        BX_SEARCH_DEV_COUNTER_ADD(path_copies_before_match, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_FILES:
        BX_SEARCH_DEV_COUNTER_ADD(search_batch_files, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_PATH_BYTES:
        BX_SEARCH_DEV_COUNTER_ADD(search_batch_path_bytes, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_ALLOCS:
        BX_SEARCH_DEV_COUNTER_ADD(search_batch_allocs, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_STORAGE_REALLOCS:
        BX_SEARCH_DEV_COUNTER_ADD(search_batch_storage_reallocs, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCH_LIFETIME_EMPTY:
        BX_SEARCH_DEV_COUNTER_ADD(search_batch_lifetime_empty, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_QUEUED:
        BX_SEARCH_DEV_COUNTER_ADD(search_batches_queued, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_SEARCHED:
        BX_SEARCH_DEV_COUNTER_ADD(search_batches_searched, count);
        return;
    case BX_SEARCH_RG_SCHED_SEARCH_BATCHES_EMPTY:
        BX_SEARCH_DEV_COUNTER_ADD(search_batches_empty, count);
        return;
    case BX_SEARCH_RG_SCHED_MEMSTREAMS_OPENED:
        BX_SEARCH_DEV_COUNTER_ADD(memstreams_opened, count);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_RECORDS_SUBMITTED:
        BX_SEARCH_DEV_COUNTER_ADD(output_records_submitted, count);
        return;
    case BX_SEARCH_RG_SCHED_DIAGNOSTIC_RECORDS_SUBMITTED:
        BX_SEARCH_DEV_COUNTER_ADD(diagnostic_records_submitted, count);
        return;
    case BX_SEARCH_RG_SCHED_MATCH_RECORDS_SUBMITTED:
        BX_SEARCH_DEV_COUNTER_ADD(match_records_submitted, count);
        return;
    case BX_SEARCH_RG_SCHED_ORDERED_OUTPUT_RECORDS:
        BX_SEARCH_DEV_COUNTER_ADD(ordered_output_records, count);
        return;
    case BX_SEARCH_RG_SCHED_UNORDERED_OUTPUT_FLUSHES:
        BX_SEARCH_DEV_COUNTER_ADD(unordered_output_flushes, count);
        return;
    case BX_SEARCH_RG_SCHED_SKIPPED_OUTPUT_SEQS:
        BX_SEARCH_DEV_COUNTER_ADD(skipped_output_seqs, count);
        return;
    case BX_SEARCH_RG_SCHED_LOCAL_FILES_SEARCHED:
        BX_SEARCH_DEV_COUNTER_ADD(local_files_searched, count);
        return;
    case BX_SEARCH_RG_SCHED_STOLEN_FILES_SEARCHED:
        BX_SEARCH_DEV_COUNTER_ADD(stolen_files_searched, count);
        return;
    case BX_SEARCH_RG_SCHED_LOCAL_DIRS_WALKED:
        BX_SEARCH_DEV_COUNTER_ADD(local_dirs_walked, count);
        return;
    case BX_SEARCH_RG_SCHED_STOLEN_DIRS_WALKED:
        BX_SEARCH_DEV_COUNTER_ADD(stolen_dirs_walked, count);
        return;
    case BX_SEARCH_RG_SCHED_QUEUED_OUTPUT_BATCHES:
        BX_SEARCH_DEV_COUNTER_ADD(queued_output_batches, count);
        return;
    case BX_SEARCH_RG_SCHED_EMPTY_OUTPUT_BATCHES:
        BX_SEARCH_DEV_COUNTER_ADD(empty_output_batches, count);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_RECORDS:
        BX_SEARCH_DEV_COUNTER_ADD(output_batch_records, count);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_STDOUT_BYTES:
        BX_SEARCH_DEV_COUNTER_ADD(output_batch_stdout_bytes, count);
        return;
    case BX_SEARCH_RG_SCHED_OUTPUT_BATCH_STDERR_BYTES:
        BX_SEARCH_DEV_COUNTER_ADD(output_batch_stderr_bytes, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SUBTREES_DONATED:
        BX_SEARCH_DEV_COUNTER_ADD(worker_subtrees_donated, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_SUBTREES_STOLEN:
        BX_SEARCH_DEV_COUNTER_ADD(worker_subtrees_stolen, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPENED:
        BX_SEARCH_DEV_COUNTER_ADD(worker_donated_dir_opened, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPEN_FAILURES:
        BX_SEARCH_DEV_COUNTER_ADD(worker_donated_dir_open_failures, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OWNED_WALKS:
        BX_SEARCH_DEV_COUNTER_ADD(worker_donated_dir_owned_walks, count);
        return;
    case BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_REOPEN_FALLBACKS:
        BX_SEARCH_DEV_COUNTER_ADD(worker_donated_dir_reopen_fallbacks, count);
        return;
    }
}

uint64_t bx_search_dev_batch_debug_next_id(void) {
    uint64_t id;

    if (!current_dev_counters.batch_debug_enabled)
        return 0u;

    pthread_mutex_lock(&current_dev_counters.lock);
    id = current_dev_counters.batch_debug_next_id++;
    pthread_mutex_unlock(&current_dev_counters.lock);
    return id;
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
    struct bx_search_dev_counter_values snapshot;

    if (!current_dev_counters.enabled || !stream)
        return;

    bx_search_dev_counters_reduce(&snapshot);

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
            snapshot.bytes_read,
            snapshot.files_opened,
            (uintmax_t)snapshot.content_open_calls,
            (uintmax_t)snapshot.content_close_calls,
            (uintmax_t)snapshot.content_fstat_calls,
            (uintmax_t)snapshot.content_fcntl_calls,
            (uintmax_t)snapshot.content_read_calls,
            (uintmax_t)snapshot.content_read_bytes,
            (uintmax_t)snapshot.content_pread_calls,
            (uintmax_t)snapshot.content_pread_bytes,
            (uintmax_t)snapshot.prefix_pread_calls,
            (uintmax_t)snapshot.prefix_pread_bytes,
            (uintmax_t)snapshot.prefix_bytes_rescanned,
            (uintmax_t)snapshot.transform_prefix_checks,
            (uintmax_t)snapshot.binary_prefix_checks,
            (uintmax_t)snapshot.files_cut_off_by_binary_prefix,
            (uintmax_t)snapshot.candidate_triggered_reopen_calls,
            (uintmax_t)snapshot.candidate_triggered_scanner_entries,
            (uintmax_t)snapshot.raw_fd_to_scanner_entries,
            (uintmax_t)snapshot.raw_fd_to_output_entries,
            (uintmax_t)snapshot.raw_fd_to_diagnostic_entries,
            snapshot.candidate_hits,
            snapshot.literal_candidate_hits,
            snapshot.literal_confirm_calls,
            snapshot.literal_matches,
            snapshot.literal_not_found,
            snapshot.literal_overlap_bytes_scanned,
            snapshot.literal_cross_chunk_matches,
            snapshot.literal_plan_compiles,
            snapshot.literal_selected_pair_start,
            snapshot.literal_selected_pair_interior,
            snapshot.literal_selected_pair_end,
            snapshot.literal_algo_empty_calls,
            snapshot.literal_algo_byte_calls,
            snapshot.literal_algo_pair_calls,
            snapshot.literal_algo_short_calls,
            snapshot.literal_algo_rare_pair_calls,
            snapshot.literal_algo_long_calls,
            snapshot.literal_algo_scalar_calls,
            snapshot.literal_algo_x86_avx2_calls,
            snapshot.literal_algo_arm64_neon_calls,
            snapshot.literal_algo_arm64_sve_calls,
            snapshot.literal_algo_memmem_calls,
            snapshot.literal_bytes_scanned,
            snapshot.literal_rare_pair_probe_calls,
            snapshot.literal_pair_mask_nonzero,
            (uintmax_t)snapshot.literal_backend_requested,
            (uintmax_t)snapshot.literal_backend_resolved,
            (uintmax_t)snapshot.literal_avx2_runtime_available,
            (uintmax_t)snapshot.literal_avx2_target_available,
            (uintmax_t)snapshot.literal_avx2_eligible_but_not_selected,
            snapshot.literal_algo_sse2_calls,
            snapshot.literal_sse2_first_last_calls,
            snapshot.matcher_invocations,
            snapshot.records_materialized,
            snapshot.scanner_entries,
            snapshot.scanner_entries_from_literal_candidate,
            snapshot.scanner_entries_without_candidate,
            snapshot.lines_counted,
            snapshot.line_boundaries_recovered,
            snapshot.candidate_triggered_line_recovery_reread_bytes,
            snapshot.records_expanded,
            snapshot.plain_line_outputs,
            snapshot.context_buffer_entries,
            snapshot.scanner_plain_prefix_allocs,
            snapshot.output_lines_emitted,
            (uintmax_t)snapshot.binary_policy_checks,
            (uintmax_t)snapshot.display_path_borrows,
            (uintmax_t)snapshot.display_path_copies,
            (uintmax_t)snapshot.display_path_copy_bytes);

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
            (uintmax_t)snapshot.walk_dirents_seen,
            (uintmax_t)snapshot.walk_getdents64_calls,
            (uintmax_t)snapshot.walk_getdents64_bytes,
            (uintmax_t)snapshot.walk_dirs_seen,
            (uintmax_t)snapshot.walk_files_seen,
            (uintmax_t)snapshot.walk_symlinks_seen,
            (uintmax_t)snapshot.walk_unknown_dtype_seen,
            (uintmax_t)snapshot.walk_stat_calls,
            (uintmax_t)snapshot.walk_fstat_calls,
            (uintmax_t)snapshot.walk_lstat_calls,
            (uintmax_t)snapshot.walk_fstatat_calls,
            (uintmax_t)snapshot.walk_stat_reason_unknown_dtype,
            (uintmax_t)snapshot.walk_stat_reason_symlink_policy,
            (uintmax_t)snapshot.walk_stat_reason_traversal_policy,
            (uintmax_t)snapshot.walk_stat_reason_metadata_filter,
            (uintmax_t)snapshot.walk_stat_reason_max_filesize,
            (uintmax_t)snapshot.walk_stat_reason_min_filesize,
            (uintmax_t)snapshot.walk_stat_reason_type,
            (uintmax_t)snapshot.walk_stat_reason_sort,
            (uintmax_t)snapshot.walk_stat_reason_metadata_output,
            (uintmax_t)snapshot.walk_stat_reason_explicit_operand,
            (uintmax_t)snapshot.walk_openat_calls,
            (uintmax_t)snapshot.walk_path_join_calls,
            (uintmax_t)snapshot.walk_path_push_calls,
            (uintmax_t)snapshot.walk_path_push_ns,
            (uintmax_t)snapshot.walk_path_pop_calls,
            (uintmax_t)snapshot.walk_path_pop_ns,
            (uintmax_t)snapshot.walk_path_allocs,
            (uintmax_t)snapshot.walk_path_copies_before_match,
            (uintmax_t)snapshot.walk_ignore_checks,
            (uintmax_t)snapshot.walk_ignore_literal_basename_checks,
            (uintmax_t)snapshot.walk_ignore_literal_basename_rejects,
            (uintmax_t)snapshot.walk_ignore_literal_extension_checks,
            (uintmax_t)snapshot.walk_ignore_literal_extension_rejects,
            (uintmax_t)snapshot.walk_ignore_anchored_prefix_checks,
            (uintmax_t)snapshot.walk_ignore_anchored_prefix_rejects,
            (uintmax_t)snapshot.walk_ignore_basename_only_fast_paths,
            (uintmax_t)snapshot.walk_ignore_no_generic_glob_fast_paths,
            (uintmax_t)snapshot.walk_ignore_builtin_checks,
            (uintmax_t)snapshot.walk_ignore_builtin_rejects,
            (uintmax_t)snapshot.walk_ignore_gitignore_checks,
            (uintmax_t)snapshot.walk_ignore_gitignore_rejects,
            (uintmax_t)snapshot.walk_ignore_dotignore_checks,
            (uintmax_t)snapshot.walk_ignore_dotignore_rejects,
            (uintmax_t)snapshot.walk_ignore_glob_fallbacks,
            (uintmax_t)snapshot.walk_ignore_generic_glob_checks,
            (uintmax_t)snapshot.walk_ignore_generic_glob_rejects,
            (uintmax_t)snapshot.walk_ignore_git_root_lstat_calls,
            (uintmax_t)snapshot.walk_ignore_git_root_lstat_misses,
            (uintmax_t)snapshot.walk_filter_ns,
            (uintmax_t)snapshot.walk_ignore_state_pushes,
            (uintmax_t)snapshot.walk_ignore_state_inline_frames,
            (uintmax_t)snapshot.walk_ignore_state_fast_paths,
            (uintmax_t)snapshot.walk_ignore_state_ns,
            (uintmax_t)snapshot.walk_filter_hidden_policy_checks,
            (uintmax_t)snapshot.walk_filter_hidden_policy_rejects,
            (uintmax_t)snapshot.walk_filter_type_policy_checks,
            (uintmax_t)snapshot.walk_filter_type_policy_rejects,
            (uintmax_t)snapshot.walk_filter_cli_glob_checks,
            (uintmax_t)snapshot.walk_filter_cli_glob_rejects,
            (uintmax_t)snapshot.walk_filter_rejected_entries,
            (uintmax_t)snapshot.walk_filter_rejected_dirs,
            (uintmax_t)snapshot.files_seen,
            (uintmax_t)snapshot.dirs_seen,
            (uintmax_t)snapshot.global_pool_submits,
            (uintmax_t)snapshot.global_pool_pops,
            (uintmax_t)snapshot.global_queue_lock_acquires,
            (uintmax_t)snapshot.global_queue_cond_wakeups,
            (uintmax_t)snapshot.worker_slot_lock_acquires,
            (uintmax_t)snapshot.worker_wakeups,
            (uintmax_t)snapshot.path_bytes_copied,
            (uintmax_t)snapshot.path_copies_before_match,
            (uintmax_t)snapshot.search_batch_files,
            (uintmax_t)snapshot.search_batch_path_bytes,
            (uintmax_t)snapshot.search_batch_allocs,
            (uintmax_t)snapshot.search_batch_storage_reallocs,
            (uintmax_t)snapshot.search_batch_lifetime_empty,
            (uintmax_t)snapshot.search_batches_queued,
            (uintmax_t)snapshot.search_batches_searched,
            (uintmax_t)snapshot.search_batches_empty,
            (uintmax_t)snapshot.search_batches_queued,
            (uintmax_t)snapshot.search_batches_searched,
            (uintmax_t)snapshot.search_batches_empty,
            (uintmax_t)snapshot.memstreams_opened,
            (uintmax_t)snapshot.output_records_submitted,
            (uintmax_t)snapshot.diagnostic_records_submitted,
            (uintmax_t)snapshot.match_records_submitted,
            (uintmax_t)snapshot.ordered_output_records,
            (uintmax_t)snapshot.unordered_output_flushes,
            (uintmax_t)snapshot.skipped_output_seqs,
            (uintmax_t)snapshot.local_files_searched,
            (uintmax_t)snapshot.stolen_files_searched,
            (uintmax_t)snapshot.local_dirs_walked,
            (uintmax_t)snapshot.stolen_dirs_walked,
            (uintmax_t)snapshot.queued_output_batches,
            (uintmax_t)snapshot.empty_output_batches,
            (uintmax_t)snapshot.output_batch_records,
            (uintmax_t)snapshot.output_batch_stdout_bytes,
            (uintmax_t)snapshot.output_batch_stderr_bytes,
            (uintmax_t)snapshot.local_files_searched,
            (uintmax_t)snapshot.local_dirs_walked,
            (uintmax_t)snapshot.worker_subtrees_donated,
            (uintmax_t)snapshot.worker_subtrees_donated,
            (uintmax_t)snapshot.worker_subtrees_stolen,
            (uintmax_t)snapshot.worker_donated_dir_opened,
            (uintmax_t)snapshot.worker_donated_dir_open_failures,
            (uintmax_t)snapshot.worker_donated_dir_owned_walks,
            (uintmax_t)snapshot.worker_donated_dir_reopen_fallbacks,
            (uintmax_t)snapshot.global_pool_submits,
            (uintmax_t)snapshot.global_pool_pops,
            (uintmax_t)snapshot.ordered_output_records,
            (uintmax_t)snapshot.unordered_output_flushes);

    fprintf(stream,
            "walk_dir_bucket_tiny_dirs=%" PRIuMAX " walk_dir_bucket_tiny_entries=%" PRIuMAX " walk_dir_bucket_tiny_ns=%" PRIuMAX " "
            "walk_dir_bucket_small_dirs=%" PRIuMAX " walk_dir_bucket_small_entries=%" PRIuMAX " walk_dir_bucket_small_ns=%" PRIuMAX " "
            "walk_dir_bucket_medium_dirs=%" PRIuMAX " walk_dir_bucket_medium_entries=%" PRIuMAX " walk_dir_bucket_medium_ns=%" PRIuMAX " "
            "walk_dir_bucket_huge_dirs=%" PRIuMAX " walk_dir_bucket_huge_entries=%" PRIuMAX " walk_dir_bucket_huge_ns=%" PRIuMAX "\n",
            (uintmax_t)snapshot.walk_dir_bucket_tiny_dirs,
            (uintmax_t)snapshot.walk_dir_bucket_tiny_entries,
            (uintmax_t)snapshot.walk_dir_bucket_tiny_ns,
            (uintmax_t)snapshot.walk_dir_bucket_small_dirs,
            (uintmax_t)snapshot.walk_dir_bucket_small_entries,
            (uintmax_t)snapshot.walk_dir_bucket_small_ns,
            (uintmax_t)snapshot.walk_dir_bucket_medium_dirs,
            (uintmax_t)snapshot.walk_dir_bucket_medium_entries,
            (uintmax_t)snapshot.walk_dir_bucket_medium_ns,
            (uintmax_t)snapshot.walk_dir_bucket_huge_dirs,
            (uintmax_t)snapshot.walk_dir_bucket_huge_entries,
            (uintmax_t)snapshot.walk_dir_bucket_huge_ns);
}
