#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <pthread.h>

#include "lib/cancel_state.h"
#include "lib/thread_count.h"
#include "lib/work_pool.h"
#include "dev_counters.h"
#include "record_stream.h"
#include "rg_parallel.h"
#include "rg_publish.h"
#include "scanner.h"
#include "search_internal.h"
#include "sort.h"
#include "traverse.h"

/*
 * Recursive rg parallel search keeps output ordered by path discovery order.
 * Batch multiple files into one worker record so tiny-file trees do not pay
 * one pool job plus one ordered-output record per file.
 */
#define BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES 64u
#define BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES 16384u

struct bx_search_parallel_job_item {
    const char *path;
    size_t path_offset;
    bool path_in_storage;
    bool strip_dot_prefix;
};

struct bx_search_parallel_job {
    uint64_t seq;
    uint64_t debug_id;
    size_t count;
    size_t path_bytes;
    size_t search_path_bytes;
    size_t storage_len;
    size_t storage_cap;
    char *storage;
    struct bx_search_parallel_job_item items[BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES];
};

struct bx_search_parallel_worker {
    struct bx_matcher *matcher;
    struct bx_search_scanner scanner;
    struct bx_record_stream record_stream;
};

struct bx_search_parallel_state {
    const char *progname;
    const char *pattern;
    enum bx_search_personality personality;
    const struct bx_search_exec_plan *exec_plan;
    struct search_opts *opts;
    struct bx_cancel_state cancel;
    struct bx_work_pool *pool;
    struct bx_rg_publish_state *publish;
    pthread_mutex_t lock;
    uint64_t next_seq;
    int exit_status;
    bool match_seen;
    bool error_seen;
    bool heading_output_started;
    bool fatal_error;
    struct bx_search_stats stats;
    struct bx_rg_publish_aggregate aggregate;
    char *fatal_message;
    struct bx_search_parallel_job *pending_job;
};

struct bx_search_parallel_walk_state {
    struct bx_search_parallel_state *parallel;
    bool strip_dot_prefix;
};

static void bx_search_parallel_set_fatal(struct bx_search_parallel_state *state,
                                         const char *message) {
    if (!state)
        return;

    pthread_mutex_lock(&state->lock);
    state->fatal_error = true;
    if (!state->fatal_message && message)
        state->fatal_message = strdup(message);
    pthread_mutex_unlock(&state->lock);

    bx_cancel_state_request(&state->cancel);
    if (state->pool) {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_WAKEUPS, 1u);
        bx_work_pool_wake(state->pool);
    }
    if (state->publish)
        bx_rg_publish_wake(state->publish);
}

static void bx_search_parallel_free_job(void *user, void *job_ptr) {
    (void)user;
    struct bx_search_parallel_job *job = job_ptr;

    if (!job)
        return;
    bx_search_dev_batch_debug_search("rg_parallel",
                                     "free",
                                     job->debug_id,
                                     (uint64_t)job->count,
                                     (uint64_t)job->search_path_bytes);
    if (job->count == 0u)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_LIFETIME_EMPTY,
                                             1u);
    free(job->storage);
    free(job);
}

static void bx_search_parallel_dispose_record(void *user,
                                              struct bx_rg_publish_record *record) {
    (void)user;
    bx_rg_publish_dispose_record(record);
}

static uint64_t bx_search_parallel_record_seq(const struct bx_rg_publish_record *record,
                                              void *user) {
    (void)user;
    return record->seq;
}

static void bx_search_parallel_emit_record(void *user,
                                           struct bx_rg_publish_record *record) {
    struct bx_search_parallel_state *state = user;

    if (!state)
        return;
    bx_rg_publish_emit_record_default(&state->aggregate, record);
}

static void bx_search_parallel_merge_direct_result(struct bx_search_parallel_state *state,
                                                   const struct bx_search_stats *stats,
                                                   bool match_seen,
                                                   bool error_seen) {
    if (!state || !state->publish)
        return;

    pthread_mutex_lock(&state->publish->unordered_lock);
    if (stats) {
        state->stats.matches += stats->matches;
        state->stats.matched_lines += stats->matched_lines;
        state->stats.files_with_matches += stats->files_with_matches;
        state->stats.files_searched += stats->files_searched;
        state->stats.bytes_printed += stats->bytes_printed;
        state->stats.bytes_searched += stats->bytes_searched;
    }
    if (match_seen) {
        state->match_seen = true;
        if (state->exit_status != 2)
            state->exit_status = 0;
    }
    if (error_seen) {
        state->error_seen = true;
        state->exit_status = 2;
    }
    pthread_mutex_unlock(&state->publish->unordered_lock);
}

static bool bx_search_parallel_submit_record(struct bx_search_parallel_state *state,
                                             struct bx_rg_publish_record *record) {
    if (!state || !record)
        return false;
    if (bx_rg_publish_submit(state->publish, record))
        return true;

    bx_search_parallel_set_fatal(state, "rg: failed to submit ordered output record\n");
    bx_search_parallel_dispose_record(NULL, record);
    return false;
}

static void bx_search_parallel_drop_empty_pending_job(struct bx_search_parallel_state *state) {
    if (!state || !state->pending_job || state->pending_job->count != 0u)
        return;

    bx_search_parallel_free_job(NULL, state->pending_job);
    state->pending_job = NULL;
}

static bool bx_search_parallel_job_reserve_storage(struct bx_search_parallel_job *job,
                                                   size_t needed) {
    char *tmp;
    size_t new_cap;

    if (!job)
        return false;
    if (job->storage_cap >= needed)
        return true;

    new_cap = job->storage_cap == 0u ? BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES
                                     : job->storage_cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    tmp = realloc(job->storage, new_cap);
    if (!tmp)
        return false;
    job->storage = tmp;
    job->storage_cap = new_cap;
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_STORAGE_REALLOCS, 1u);
    return true;
}

static bool bx_search_parallel_job_store_string_len(struct bx_search_parallel_job *job,
                                                    const char *text,
                                                    size_t len_with_nul,
                                                    size_t *offset_out) {
    size_t offset;

    if (!job || !text || len_with_nul == 0u || !offset_out)
        return false;
    if (!bx_search_parallel_job_reserve_storage(job, job->storage_len + len_with_nul))
        return false;

    offset = job->storage_len;
    memcpy(job->storage + offset, text, len_with_nul);
    job->storage_len += len_with_nul;
    *offset_out = offset;
    return true;
}

static const char *bx_search_parallel_job_item_path(const struct bx_search_parallel_job *job,
                                                    const struct bx_search_parallel_job_item *item) {
    if (!job || !item)
        return NULL;
    if (!item->path_in_storage)
        return item->path;
    if (!job->storage || item->path_offset >= job->storage_len)
        return NULL;
    return job->storage + item->path_offset;
}

static bool bx_search_parallel_flush_pending_job(struct bx_search_parallel_state *state) {
    struct bx_search_parallel_job *job;

    if (!state || !state->pending_job)
        return true;

    job = state->pending_job;
    state->pending_job = NULL;
    if (job->count == 0u) {
        bx_search_parallel_free_job(NULL, job);
        return true;
    }
    job->seq = state->next_seq++;
    if (bx_work_pool_submit(state->pool, job)) {
        bx_search_dev_batch_debug_search("rg_parallel",
                                         "queued",
                                         job->debug_id,
                                         (uint64_t)job->count,
                                         (uint64_t)job->search_path_bytes);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_FILES,
                                             (uint64_t)job->count);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_PATH_BYTES,
                                             (uint64_t)job->search_path_bytes);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_QUEUED, 1u);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_GLOBAL_POOL_SUBMITS, 1u);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_WAKEUPS, 1u);
        return true;
    }

    bx_search_parallel_free_job(NULL, job);
    return false;
}

static bool bx_search_parallel_submit_path_error(struct bx_search_parallel_state *state,
                                                 const char *path,
                                                 int errnum,
                                                 bool io_operation_style) {
    struct bx_rg_publish_record *record = calloc(1u, sizeof(*record));
    FILE *err_stream = NULL;

    if (!record)
        return false;
    if (!bx_search_parallel_flush_pending_job(state)) {
        bx_search_parallel_dispose_record(NULL, record);
        return false;
    }
    record->seq = state->next_seq;
    record->status = 2;
    record->error_seen = true;

    if (state->opts && state->opts->suppress_errors) {
        if (!bx_search_parallel_submit_record(state, record))
            return false;
        state->next_seq++;
        return true;
    }

    err_stream = open_memstream(&record->stderr_buf, &record->stderr_len);
    if (!err_stream) {
        bx_search_parallel_dispose_record(NULL, record);
        return false;
    }

    if (io_operation_style)
        bx_search_fprintf_path_io_error(err_stream, state->progname, path, errnum);
    else
        bx_search_fprintf_path_error(err_stream, state->progname, path, errnum);
    fclose(err_stream);

    if (!bx_search_parallel_submit_record(state, record))
        return false;
    state->next_seq++;
    return true;
}

static bool bx_search_parallel_queue_path(struct bx_search_parallel_state *state,
                                          const char *path,
                                          bool path_copy_required,
                                          bool strip_dot_prefix) {
    struct bx_search_parallel_job *job;
    struct bx_search_parallel_job_item *item;
    size_t item_cost;
    size_t path_len;

    if (!state || !path)
        return false;
    if (bx_cancel_state_requested(&state->cancel))
        return false;

    path_len = strlen(path) + 1u;
    item_cost = path_copy_required ? path_len : 0u;
    job = state->pending_job;
    if (job &&
        (job->count >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES ||
         (job->count > 0u &&
          job->path_bytes + item_cost > BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES))) {
        if (!bx_search_parallel_flush_pending_job(state))
            return false;
        job = NULL;
    }

    if (!job) {
        job = calloc(1u, sizeof(*job));
        if (!job)
            return false;
        job->debug_id = bx_search_dev_batch_debug_next_id();
        bx_search_dev_batch_debug_search("rg_parallel", "alloc", job->debug_id, 0u, 0u);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_ALLOCS, 1u);
        state->pending_job = job;
    }

    item = &job->items[job->count];
    if (path_copy_required) {
        if (!bx_search_parallel_job_store_string_len(job, path, path_len, &item->path_offset)) {
            bx_search_parallel_drop_empty_pending_job(state);
            return false;
        }
        item->path = NULL;
        item->path_in_storage = true;
    } else {
        item->path = path;
        item->path_offset = 0u;
        item->path_in_storage = false;
    }

    job->count++;
    item->strip_dot_prefix = strip_dot_prefix;
    job->path_bytes += item_cost;
    job->search_path_bytes += path_len;
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_FILES_SEEN, 1u);
    if (item_cost > 0u) {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_PATH_BYTES_COPIED,
                                             (uint64_t)item_cost);
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_PATH_COPIES_BEFORE_MATCH, 1u);
    }

    if (job->count >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_FILES ||
        job->path_bytes >= BX_SEARCH_PARALLEL_JOB_BATCH_MAX_PATH_BYTES)
        return bx_search_parallel_flush_pending_job(state);
    return true;
}

static void *bx_search_parallel_worker_init(void *user, size_t worker_index) {
    struct bx_search_parallel_state *state = user;
    struct bx_search_parallel_worker *worker;
    char *errmsg = NULL;

    (void)worker_index;
    worker = calloc(1u, sizeof(*worker));
    if (!worker)
        return NULL;

    worker->matcher = bx_search_compile_matcher(state->pattern, state->personality,
                                                state->opts, &errmsg, NULL);
    if (!worker->matcher) {
        if (errmsg) {
            bx_search_parallel_set_fatal(state, errmsg);
            free(errmsg);
        }
        free(worker);
        return NULL;
    }

    return worker;
}

static void bx_search_parallel_worker_fini(void *user, void *worker_local, size_t worker_index) {
    (void)user;
    (void)worker_index;
    struct bx_search_parallel_worker *worker = worker_local;

    if (!worker)
        return;
    bx_search_matcher_free(worker->matcher);
    bx_search_scanner_dispose(&worker->scanner);
    bx_record_stream_dispose(&worker->record_stream);
    free(worker);
}

static void bx_search_parallel_process_job(void *user,
                                           void *worker_local,
                                           void *job_ptr,
                                           size_t worker_index) {
    struct bx_search_parallel_state *state = user;
    struct bx_search_parallel_worker *worker = worker_local;
    struct bx_search_parallel_job *job = job_ptr;
    struct bx_rg_publish_record *record = NULL;
    char *stdout_buf = NULL;
    size_t stdout_len = 0u;
    char *stderr_buf = NULL;
    size_t stderr_len = 0u;
    struct bx_search_stats job_stats = {0};
    struct bx_search_output_ctx output_ctx = {0};
    struct bx_search_output_ctx *previous_ctx = NULL;
    int match_count = 0;
    int job_status = 1;
    bool job_match_seen = false;
    bool job_error_seen = false;

    (void)worker_index;
    if (!state || !worker || !job)
        return;
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_GLOBAL_POOL_POPS, 1u);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_SEARCHED, 1u);
    bx_search_dev_batch_debug_search("rg_parallel",
                                     "searched",
                                     job->debug_id,
                                     (uint64_t)job->count,
                                     (uint64_t)job->search_path_bytes);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_STOLEN_FILES_SEARCHED,
                                         (uint64_t)job->count);

    if (bx_cancel_state_requested(&state->cancel) && state->opts->quiet) {
        bx_search_parallel_free_job(NULL, job);
        return;
    }
    output_ctx.capture_out_buf = &stdout_buf;
    output_ctx.capture_out_len = &stdout_len;
    output_ctx.capture_err_buf = &stderr_buf;
    output_ctx.capture_err_len = &stderr_len;
    output_ctx.stats = state->opts->stats ? &job_stats : NULL;
    previous_ctx = bx_search_output_ctx_push(&output_ctx);
    for (size_t i = 0; i < job->count; i++) {
        int status;
        const char *path;

        if (state->opts->quiet && bx_cancel_state_requested(&state->cancel))
            break;

        path = bx_search_parallel_job_item_path(job, &job->items[i]);
        if (!path) {
            bx_search_parallel_set_fatal(state, "rg: failed to resolve queued worker path\n");
            job_error_seen = true;
            job_status = 2;
            break;
        }

        status = bx_search_search_file(path,
                                       NULL,
                                       job->items[i].strip_dot_prefix,
                                       state->progname,
                                       worker->matcher,
                                       state->exec_plan,
                                       state->opts,
                                       &match_count,
                                       &worker->scanner,
                                       &worker->record_stream,
                                       &job_stats);
        if (status == 2) {
            job_error_seen = true;
            job_status = 2;
            continue;
        }
        if (status == 0) {
            job_match_seen = true;
            if (job_status != 2)
                job_status = 0;
            if (state->opts->quiet && bx_cancel_state_request(&state->cancel)) {
                bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_WAKEUPS, 1u);
                bx_work_pool_wake(state->pool);
                break;
            }
        }
    }
    bx_search_output_ctx_pop(previous_ctx);

    if (output_ctx.out)
        fclose(output_ctx.out);
    if (output_ctx.err)
        fclose(output_ctx.err);
    if (output_ctx.capture_failed) {
        bx_search_parallel_set_fatal(state, "rg: failed to allocate worker output streams\n");
        free(stdout_buf);
        free(stderr_buf);
        bx_search_parallel_free_job(NULL, job);
        return;
    }

    /*
     * Avoid queuing an output batch unless the job actually produced captured
     * output. Stats-only and no-output outcomes merge directly.
     */
    if (stdout_len == 0u && stderr_len == 0u) {
        if (!state->opts->stats && !job_match_seen && !job_error_seen) {
            bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_EMPTY, 1u);
            bx_search_dev_batch_debug_search("rg_parallel",
                                             "empty-result",
                                             job->debug_id,
                                             (uint64_t)job->count,
                                             (uint64_t)job->search_path_bytes);
        }
        if (state->opts->stats || job_match_seen || job_error_seen) {
            bx_search_parallel_merge_direct_result(state,
                                                  state->opts->stats ? &job_stats : NULL,
                                                  job_match_seen,
                                                  job_error_seen);
        }
        free(stdout_buf);
        free(stderr_buf);
        bx_search_parallel_free_job(NULL, job);
        return;
    }

    record = calloc(1u, sizeof(*record));
    if (!record) {
        free(stdout_buf);
        free(stderr_buf);
        bx_search_parallel_set_fatal(state, "rg: failed to allocate worker output record\n");
        bx_search_parallel_free_job(NULL, job);
        return;
    }
    record->seq = job->seq;
    record->stdout_buf = stdout_buf;
    record->stdout_len = stdout_len;
    record->stderr_buf = stderr_buf;
    record->stderr_len = stderr_len;
    record->stats = job_stats;
    record->status = job_status;
    record->match_seen = job_match_seen;
    record->error_seen = job_error_seen;
    record->used_heading = output_ctx.used_heading;
    if (!bx_search_parallel_submit_record(state, record))
        bx_search_parallel_set_fatal(state, "rg: failed to queue worker output\n");
    bx_search_parallel_free_job(NULL, job);
}

static enum bx_walk_action bx_search_parallel_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct bx_search_parallel_walk_state *state = user;

    if (!state || !state->parallel)
        return BX_WALK_ERROR;
    if (bx_cancel_state_requested(&state->parallel->cancel))
        return BX_WALK_STOP;
    if (entry->is_dir) {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_DIRS_SEEN, 1u);
        return BX_WALK_CONTINUE;
    }
    if (bx_search_entry_can_skip_max_filesize_zero_literal(entry,
                                                           state->parallel->exec_plan,
                                                           state->parallel->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state->parallel->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_recursive_special_input(entry, state->parallel->opts))
        return BX_WALK_CONTINUE;

    if (!bx_search_parallel_queue_path(state->parallel, entry->path, true,
                                       state->strip_dot_prefix)) {
        return bx_cancel_state_requested(&state->parallel->cancel)
            ? BX_WALK_STOP
            : BX_WALK_ERROR;
    }
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_parallel_walk_error_cb(const char *path,
                                                            int errnum,
                                                            void *user) {
    struct bx_search_parallel_walk_state *state = user;

    if (!state || !state->parallel)
        return BX_WALK_ERROR;
    if (!bx_search_parallel_submit_path_error(state->parallel, path, errnum, false))
        return BX_WALK_ERROR;
    return bx_cancel_state_requested(&state->parallel->cancel)
        ? BX_WALK_STOP
        : BX_WALK_CONTINUE;
}

static size_t bx_search_rg_auto_thread_count(const struct search_opts *opts) {
    size_t thread_count = bx_thread_count_resolve(opts ? opts->threads : 0);

    if (!opts || opts->threads > 0 || thread_count <= 4u)
        return thread_count;
    return 4u;
}

size_t bx_search_rg_thread_count(const struct search_opts *opts) {
    return bx_search_rg_auto_thread_count(opts);
}

bool bx_search_parallel_rg_supported(enum bx_search_personality personality,
                                     const struct search_opts *opts,
                                     int num_files,
                                     bool rg_searches_stdin) {
    if (!opts || personality != BX_SEARCH_RG)
        return false;
    if (opts->files_only || opts->trace || opts->quiet || rg_searches_stdin)
        return false;
    if (bx_search_sort_requested(opts))
        return false;
    if (bx_search_rg_auto_thread_count(opts) <= 1u)
        return false;
    if (num_files == 0)
        return true;
    return opts->recursive || num_files > 1;
}

int bx_search_run_parallel_rg(int argc,
                              char **argv,
                              int first_file,
                              struct bx_search_operand_ref *sorted_operands,
                              int sorted_operand_count,
                              const char *progname,
                              const char *pattern,
                              enum bx_search_personality personality,
                              const struct bx_search_exec_plan *exec_plan,
                              struct search_opts *opts,
                              struct bx_search_stats *stats_out,
                              bool *match_seen_out,
                              bool *error_seen_out) {
    struct bx_search_parallel_state state = {
        .progname = progname,
        .pattern = pattern,
        .personality = personality,
        .exec_plan = exec_plan,
        .opts = opts,
        .exit_status = 1,
    };
    struct bx_work_pool pool = {0};
    struct bx_rg_publish_state publish = {0};
    size_t thread_count = bx_search_rg_auto_thread_count(opts);
    size_t queue_capacity = thread_count > (SIZE_MAX / 64u) ? thread_count : thread_count * 64u;
    int num_files = argc - first_file;
    bool pool_ready = false;
    bool publish_ready = false;
    bool walk_error_seen = false;

    if (queue_capacity < thread_count)
        queue_capacity = thread_count;

    bx_cancel_state_init(&state.cancel);
    if (pthread_mutex_init(&state.lock, NULL) != 0)
        return 2;
    state.aggregate = (struct bx_rg_publish_aggregate){
        .stats = &state.stats,
        .exit_status = &state.exit_status,
        .match_seen = &state.match_seen,
        .error_seen = &state.error_seen,
        .heading_output_started = &state.heading_output_started,
    };

    struct bx_rg_publish_opts publish_opts = {
        .mode = BX_RG_PUBLISH_UNORDERED,
        .max_pending = queue_capacity,
        .first_seq = 0u,
        .user = &state,
        .record_seq = bx_search_parallel_record_seq,
        .emit_record = bx_search_parallel_emit_record,
        .dispose_record = bx_search_parallel_dispose_record,
    };
    if (!bx_rg_publish_init(&publish, &publish_opts)) {
        pthread_mutex_destroy(&state.lock);
        return 2;
    }
    publish_ready = true;
    state.publish = &publish;

    struct bx_work_pool_opts pool_opts = {
        .thread_count = thread_count,
        .queue_capacity = queue_capacity,
        .user = &state,
        .cancel = &state.cancel,
        .worker_init = bx_search_parallel_worker_init,
        .worker_fini = bx_search_parallel_worker_fini,
        .process_job = bx_search_parallel_process_job,
        .dispose_job = bx_search_parallel_free_job,
    };
    if (!bx_work_pool_init(&pool, &pool_opts)) {
        bx_search_parallel_set_fatal(&state, "rg: failed to initialize worker pool\n");
        goto done;
    }
    pool_ready = true;
    state.pool = &pool;

    if (num_files == 0) {
        struct bx_search_parallel_walk_state walk_state = {
            .parallel = &state,
            .strip_dot_prefix = true,
        };
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, opts, NULL);
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = bx_search_parallel_walk_cb,
            .error = bx_search_parallel_walk_error_cb,
        };

        if (bx_search_walk(".", &walk_config, &walk_state) != 0)
            walk_error_seen = true;
    } else if (opts->recursive) {
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, opts, NULL);
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, opts);
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit = bx_search_parallel_walk_cb,
            .error = bx_search_parallel_walk_error_cb,
        };
        struct bx_search_parallel_walk_state walk_state = {
            .parallel = &state,
            .strip_dot_prefix = false,
        };

        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j;
            struct stat st;

            if (bx_cancel_state_requested(&state.cancel))
                break;
            j = sorted_operands
                    ? sorted_operands[bx_search_sort_is_descending(opts)
                                          ? (sorted_operand_count - 1 - operand_i)
                                          : operand_i]
                          .index
                    : (first_file + operand_i);
            if (stat(argv[j], &st) != 0) {
                if (!bx_search_parallel_submit_path_error(&state, argv[j], errno,
                                                          num_files == 1)) {
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue traversal error\n");
                    break;
                }
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (bx_search_walk(argv[j], &walk_config, &walk_state) != 0)
                    walk_error_seen = true;
                continue;
            }
            if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                continue;
            if (!bx_search_explicit_entry_selected(opts, argv[j]))
                continue;
            if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                continue;

            if (!bx_search_parallel_queue_path(&state, argv[j], false, false)) {
                if (!bx_cancel_state_requested(&state.cancel))
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
                break;
            }
        }
    } else {
        for (int operand_i = 0; operand_i < num_files; operand_i++) {
            int j = sorted_operands
                        ? sorted_operands[bx_search_sort_is_descending(opts)
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            if (argv[j] && strcmp(argv[j], "-") != 0) {
                struct stat st;
                if (lstat(argv[j], &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        if (!bx_search_parallel_submit_path_error(&state, argv[j], EISDIR,
                                                                  false))
                            bx_search_parallel_set_fatal(&state, "rg: failed to queue directory error\n");
                        continue;
                    }
                    if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                        continue;
                }
                if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                    continue;
            }
            if (!bx_search_parallel_queue_path(&state, argv[j], false, false)) {
                if (!bx_cancel_state_requested(&state.cancel))
                    bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
                break;
            }
        }
    }

done:
    if (!state.fatal_error && !bx_search_parallel_flush_pending_job(&state))
        bx_search_parallel_set_fatal(&state, "rg: failed to queue file job\n");
    if (pool_ready) {
        bx_work_pool_close(&pool);
        if (!bx_work_pool_join(&pool) && !state.fatal_error)
            bx_search_parallel_set_fatal(&state, "rg: worker pool failed\n");
    }
    if (publish_ready) {
        bx_rg_publish_close(&publish);
        bx_rg_publish_join(&publish);
    }
    if (state.fatal_error) {
        state.error_seen = true;
        state.exit_status = 2;
        if (state.fatal_message && *state.fatal_message) {
            fputs(state.fatal_message, stderr);
            if (state.fatal_message[strlen(state.fatal_message) - 1] != '\n')
                fputc('\n', stderr);
        }
    }
    if (walk_error_seen) {
        state.error_seen = true;
        state.exit_status = 2;
    }

    if (stats_out)
        *stats_out = state.stats;
    if (match_seen_out)
        *match_seen_out = state.match_seen;
    if (error_seen_out)
        *error_seen_out = state.error_seen;

    if (pool_ready)
        bx_work_pool_dispose(&pool);
    if (publish_ready)
        bx_rg_publish_dispose(&publish);
    if (state.pending_job)
        bx_search_parallel_free_job(NULL, state.pending_job);
    pthread_mutex_destroy(&state.lock);
    free(state.fatal_message);
    return state.exit_status;
}
