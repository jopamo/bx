#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pthread.h>

#include "dev_counters.h"
#include "lib/cancel_state.h"
#include "lib/fd_ops.h"
#include "lib/thread_count.h"
#include "record_stream.h"
#include "rg_output.h"
#include "rg_parallel.h"
#include "rg_publish.h"
#include "rg_sched.h"
#include "scanner.h"
#include "search_internal.h"
#include "search_plan.h"
#include "sort.h"
#include "traverse.h"

#define BX_RG_SCHED_BATCH_MAX_FILES 64u

enum bx_rg_sched_work_kind {
    BX_RG_SCHED_WORK_DIR = 0,
    BX_RG_SCHED_WORK_FILE_BATCH,
};

enum bx_rg_sched_cancel_reason {
    BX_RG_SCHED_CANCEL_NONE = 0,
    BX_RG_SCHED_CANCEL_FATAL_ERROR,
};

struct bx_rg_sched_file_item {
    const char *path;
    int base_depth;
    bool strip_dot_prefix;
};

struct bx_rg_sched_work {
    enum bx_rg_sched_work_kind kind;
    uint64_t debug_id;
    int base_depth;
    bool strip_dot_prefix;
    size_t storage_len;
    size_t storage_cap;
    char *storage;
    union {
        struct {
            size_t path_offset;
            size_t git_root_offset;
            struct bx_ignore_state *parent_ignore_state_snapshot;
            DIR *owned_dir;
            bool donated_dir;
            bool git_root_resolved;
            bool gitignore_enabled;
        } dir;
        struct {
            size_t count;
            struct bx_rg_sched_file_item items[BX_RG_SCHED_BATCH_MAX_FILES];
        } batch;
    } u;
};

struct bx_rg_sched_work_vec {
    struct bx_rg_sched_work **items;
    size_t len;
    size_t cap;
};

struct bx_rg_sched_worker_slot {
    pthread_mutex_t lock;
    struct bx_rg_sched_work **items;
    size_t len;
    size_t cap;
};

struct bx_rg_sched_state {
    const char *progname;
    const char *pattern;
    enum bx_search_personality personality;
    const struct bx_search_exec_plan *exec_plan;
    struct bx_search_plan quiet_plan;
    struct search_opts *opts;
    struct search_opts quiet_opts;
    struct bx_cancel_state cancel;
    struct bx_rg_publish_state publish;
    bool publish_ready;
    pthread_mutex_t lock;
    pthread_cond_t work_ready;
    struct bx_rg_sched_worker_slot *worker_slots;
    int exit_status;
    bool match_seen;
    bool error_seen;
    bool heading_output_started;
    bool fatal_error;
    bool shutdown;
    enum bx_rg_sched_cancel_reason cancel_reason;
    struct bx_search_stats stats;
    FILE *stdout_stream;
    char *fatal_message;
    size_t thread_count;
    size_t pending_work;
    size_t active_workers;
    size_t idle_workers;
};

struct bx_rg_sched_worker {
    struct bx_matcher *matcher;
    struct bx_search_exec_plan quiet_exec_plan;
    struct bx_search_scanner scanner;
    struct bx_record_stream record_stream;
    struct bx_rg_display_path_buf display_path_buf;
    char *stdout_buf;
    size_t stdout_len;
    size_t stdout_cap;
};

struct bx_rg_sched_thread_arg {
    struct bx_rg_sched_state *sched;
    size_t worker_index;
};

struct bx_rg_sched_walk_state {
    struct bx_rg_sched_state *sched;
    struct bx_rg_sched_worker *worker;
    char **stderr_buf;
    size_t *stderr_len;
    size_t *stderr_cap;
    bool *match_seen;
    bool *error_seen;
    size_t worker_index;
    int base_depth;
    const char *work_root_path;
    bool strip_dot_prefix;
    bool stolen;
    bool defer_output_capture;
};

struct bx_rg_sched_frontier_state {
    struct bx_rg_sched_state *sched;
    struct bx_rg_sched_work_vec *vec;
    struct bx_rg_sched_work *pending_batch;
    const char *root_path;
    bool strip_dot_prefix;
};

static void bx_rg_sched_work_vec_dispose(struct bx_rg_sched_work_vec *vec);
static void bx_rg_sched_free_work(struct bx_rg_sched_work *work);

static void bx_rg_sched_lock_global(struct bx_rg_sched_state *sched) {
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_LOCK_ACQUIRES,
                                         1u);
    pthread_mutex_lock(&sched->lock);
}

static void bx_rg_sched_lock_worker_slot(struct bx_rg_sched_worker_slot *slot) {
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_SLOT_LOCK_ACQUIRES,
                                         1u);
    pthread_mutex_lock(&slot->lock);
}

static void bx_rg_sched_publish_dispose_record(void *user,
                                               struct bx_rg_publish_record *record) {
    (void)user;
    bx_rg_publish_dispose_record(record);
}

static void bx_rg_sched_signal_workers_locked(struct bx_rg_sched_state *sched) {
    if (!sched)
        return;
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_GLOBAL_QUEUE_COND_WAKEUPS,
                                         1u);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_WAKEUPS, 1u);
    pthread_cond_broadcast(&sched->work_ready);
}

static void bx_rg_sched_note_dir_walk(bool stolen) {
    bx_search_dev_counters_note_rg_sched(stolen
                                             ? BX_SEARCH_RG_SCHED_STOLEN_DIRS_WALKED
                                             : BX_SEARCH_RG_SCHED_LOCAL_DIRS_WALKED,
                                         1u);
}

static void bx_rg_sched_note_file_search(bool stolen) {
    bx_search_dev_counters_note_rg_sched(stolen
                                             ? BX_SEARCH_RG_SCHED_STOLEN_FILES_SEARCHED
                                             : BX_SEARCH_RG_SCHED_LOCAL_FILES_SEARCHED,
                                         1u);
}

static bool bx_rg_sched_uses_path_only_output(const struct search_opts *opts) {
    return opts && (opts->files_with_matches || opts->files_without_match);
}

static bool bx_rg_sched_can_defer_output_capture(const struct bx_rg_sched_state *sched) {
    /*
     * In --no-messages deferred-literal match-line mode, the first pass can
     * run as a quiet truth probe. No output stream is published unless the
     * probe finds a possible match; diagnostics are suppressed by policy.
     */
    return sched && sched->opts && sched->opts->suppress_errors &&
           !bx_rg_sched_uses_path_only_output(sched->opts) &&
           sched->exec_plan && sched->exec_plan->deferred_literal_precheck;
}

static void bx_rg_sched_apply_result_unlocked(struct bx_rg_sched_state *sched,
                                              bool match_seen,
                                              bool error_seen) {
    if (!sched)
        return;

    /*
     * Error status is sticky. Unordered match publication may arrive after an
     * earlier direct or published error, but it must not demote rc=2 to rc=0.
     */
    if (match_seen) {
        sched->match_seen = true;
        if (sched->exit_status != 2)
            sched->exit_status = 0;
    }
    if (error_seen) {
        sched->error_seen = true;
        sched->exit_status = 2;
    }
}

static void bx_rg_sched_merge_direct_result(struct bx_rg_sched_state *sched,
                                            bool match_seen,
                                            bool error_seen) {
    if (!sched)
        return;

    if (sched->publish_ready)
        pthread_mutex_lock(&sched->publish.unordered_lock);
    bx_rg_sched_apply_result_unlocked(sched, match_seen, error_seen);
    if (sched->publish_ready)
        pthread_mutex_unlock(&sched->publish.unordered_lock);
}

static void bx_rg_sched_request_cancel_locked(
    struct bx_rg_sched_state *state,
    enum bx_rg_sched_cancel_reason reason) {
    if (!state || reason == BX_RG_SCHED_CANCEL_NONE)
        return;
    if (state->cancel_reason == BX_RG_SCHED_CANCEL_NONE)
        state->cancel_reason = reason;
    state->shutdown = true;
    bx_rg_sched_signal_workers_locked(state);
}

static void bx_rg_sched_set_fatal(struct bx_rg_sched_state *state,
                                  const char *message) {
    if (!state)
        return;

    bx_rg_sched_lock_global(state);
    state->fatal_error = true;
    if (!state->fatal_message && message)
        state->fatal_message = strdup(message);
    bx_rg_sched_request_cancel_locked(state, BX_RG_SCHED_CANCEL_FATAL_ERROR);
    pthread_mutex_unlock(&state->lock);

    bx_cancel_state_request(&state->cancel);
    if (state->publish_ready)
        bx_rg_publish_wake(&state->publish);
}

static void bx_rg_sched_report_path_error(const struct bx_rg_sched_state *state,
                                          const char *path,
                                          int errnum,
                                          bool io_operation_style) {
    if (!state || (state->opts && state->opts->suppress_errors))
        return;

    if (io_operation_style)
        bx_search_fprintf_path_io_error(stderr, state->progname, path, errnum);
    else
        bx_search_fprintf_path_error(stderr, state->progname, path, errnum);
}

static void bx_rg_sched_free_work(struct bx_rg_sched_work *work) {
    if (!work)
        return;
    if (work->kind == BX_RG_SCHED_WORK_FILE_BATCH) {
        bx_search_dev_batch_debug_search("rg_sched",
                                         "free",
                                         work->debug_id,
                                         (uint64_t)work->u.batch.count,
                                         (uint64_t)work->storage_len);
    }
    if (work->kind == BX_RG_SCHED_WORK_FILE_BATCH && work->u.batch.count == 0u)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_LIFETIME_EMPTY,
                                             1u);
    if (work->kind == BX_RG_SCHED_WORK_DIR) {
        if (work->u.dir.owned_dir)
            closedir(work->u.dir.owned_dir);
        bx_ignore_state_dispose_chain(work->u.dir.parent_ignore_state_snapshot);
    }
    free(work->storage);
    free(work);
}

static bool bx_rg_sched_work_reserve_storage(struct bx_rg_sched_work *work, size_t needed) {
    if (!work)
        return false;
    if (work->storage_cap >= needed)
        return true;

    size_t new_cap = work->storage_cap == 0u ? 256u : work->storage_cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    char *tmp = realloc(work->storage, new_cap);
    if (!tmp)
        return false;
    work->storage = tmp;
    work->storage_cap = new_cap;
    if (work->kind == BX_RG_SCHED_WORK_FILE_BATCH)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_STORAGE_REALLOCS,
                                             1u);
    return true;
}

static bool bx_rg_sched_work_store_string(struct bx_rg_sched_work *work,
                                          const char *text,
                                          size_t *offset_out) {
    size_t offset;

    if (!work || !text || !offset_out)
        return false;

    size_t len = strlen(text) + 1u;
    if (!bx_rg_sched_work_reserve_storage(work, work->storage_len + len))
        return false;

    offset = work->storage_len;
    memcpy(work->storage + offset, text, len);
    work->storage_len += len;
    *offset_out = offset;
    return true;
}

static const char *bx_rg_sched_work_storage_string(const struct bx_rg_sched_work *work,
                                                   size_t offset) {
    if (!work || !work->storage || offset == SIZE_MAX || offset >= work->storage_len)
        return NULL;
    return work->storage + offset;
}

static bool bx_rg_sched_work_vec_push(struct bx_rg_sched_work_vec *vec,
                                      struct bx_rg_sched_work *work) {
    if (!vec || !work)
        return false;
    if (vec->len == vec->cap) {
        size_t new_cap = vec->cap == 0u ? 16u : vec->cap * 2u;
        struct bx_rg_sched_work **tmp = realloc(vec->items, new_cap * sizeof(*tmp));
        if (!tmp)
            return false;
        vec->items = tmp;
        vec->cap = new_cap;
    }
    vec->items[vec->len++] = work;
    return true;
}

static bool bx_rg_sched_worker_slot_reserve(struct bx_rg_sched_worker_slot *slot,
                                            size_t needed) {
    if (!slot)
        return false;
    if (slot->cap >= needed)
        return true;

    size_t new_cap = slot->cap == 0u ? 8u : slot->cap * 2u;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }

    struct bx_rg_sched_work **tmp = realloc(slot->items, new_cap * sizeof(*tmp));
    if (!tmp)
        return false;
    slot->items = tmp;
    slot->cap = new_cap;
    return true;
}

static bool bx_rg_sched_worker_slots_init(struct bx_rg_sched_state *sched) {
    if (!sched || sched->thread_count == 0u)
        return false;

    sched->worker_slots = calloc(sched->thread_count, sizeof(*sched->worker_slots));
    if (!sched->worker_slots)
        return false;

    for (size_t i = 0; i < sched->thread_count; ++i) {
        if (pthread_mutex_init(&sched->worker_slots[i].lock, NULL) != 0) {
            for (size_t j = 0; j < i; ++j)
                pthread_mutex_destroy(&sched->worker_slots[j].lock);
            free(sched->worker_slots);
            sched->worker_slots = NULL;
            return false;
        }
    }
    return true;
}

static void bx_rg_sched_worker_slots_dispose(struct bx_rg_sched_state *sched) {
    if (!sched || !sched->worker_slots)
        return;

    for (size_t i = 0; i < sched->thread_count; ++i) {
        struct bx_rg_sched_worker_slot *slot = &sched->worker_slots[i];
        for (size_t j = 0; j < slot->len; ++j)
            bx_rg_sched_free_work(slot->items[j]);
        free(slot->items);
        pthread_mutex_destroy(&slot->lock);
    }
    free(sched->worker_slots);
    sched->worker_slots = NULL;
}

static bool bx_rg_sched_enqueue_local_work_with_policy(struct bx_rg_sched_state *sched,
                                                       size_t worker_index,
                                                       struct bx_rg_sched_work *work,
                                                       bool require_idle,
                                                       bool *queued_out) {
    if (!sched || !sched->worker_slots || worker_index >= sched->thread_count || !work)
        return false;
    if (queued_out)
        *queued_out = false;

    struct bx_rg_sched_worker_slot *slot = &sched->worker_slots[worker_index];
    bx_rg_sched_lock_worker_slot(slot);
    bool ok = bx_rg_sched_worker_slot_reserve(slot, slot->len + 1u);
    if (!ok) {
        pthread_mutex_unlock(&slot->lock);
        return false;
    }

    bx_rg_sched_lock_global(sched);
    if (require_idle && (sched->idle_workers == 0u || sched->shutdown)) {
        pthread_mutex_unlock(&sched->lock);
        pthread_mutex_unlock(&slot->lock);
        return true;
    }
    slot->items[slot->len++] = work;
    sched->pending_work++;
    if (sched->idle_workers > 0u && !sched->shutdown)
        bx_rg_sched_signal_workers_locked(sched);
    pthread_mutex_unlock(&sched->lock);
    pthread_mutex_unlock(&slot->lock);
    if (work->kind == BX_RG_SCHED_WORK_DIR)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_SUBTREES_DONATED, 1u);
    if (queued_out)
        *queued_out = true;
    return true;
}

static bool bx_rg_sched_enqueue_local_work(struct bx_rg_sched_state *sched,
                                           size_t worker_index,
                                           struct bx_rg_sched_work *work) {
    return bx_rg_sched_enqueue_local_work_with_policy(sched, worker_index, work, false, NULL);
}

static bool bx_rg_sched_enqueue_local_work_if_idle(struct bx_rg_sched_state *sched,
                                                   size_t worker_index,
                                                   struct bx_rg_sched_work *work,
                                                   bool *queued_out) {
    return bx_rg_sched_enqueue_local_work_with_policy(sched,
                                                      worker_index,
                                                      work,
                                                      true,
                                                      queued_out);
}

static bool bx_rg_sched_has_idle_worker(struct bx_rg_sched_state *sched) {
    bool has_idle;

    if (!sched)
        return false;

    bx_rg_sched_lock_global(sched);
    has_idle = sched->idle_workers > 0u && !sched->shutdown;
    pthread_mutex_unlock(&sched->lock);
    return has_idle;
}

static void bx_rg_sched_note_work_claimed(struct bx_rg_sched_state *sched) {
    bx_rg_sched_lock_global(sched);
    if (sched->pending_work > 0u)
        sched->pending_work--;
    sched->active_workers++;
    pthread_mutex_unlock(&sched->lock);
}

static struct bx_rg_sched_work *bx_rg_sched_try_pop_local_work(struct bx_rg_sched_state *sched,
                                                               size_t worker_index,
                                                               bool *stolen_out) {
    if (!sched || !sched->worker_slots || worker_index >= sched->thread_count)
        return NULL;

    struct bx_rg_sched_worker_slot *slot = &sched->worker_slots[worker_index];
    bx_rg_sched_lock_worker_slot(slot);
    if (slot->len == 0u) {
        pthread_mutex_unlock(&slot->lock);
        return NULL;
    }

    struct bx_rg_sched_work *work = slot->items[slot->len - 1u];
    slot->items[slot->len - 1u] = NULL;
    slot->len--;
    pthread_mutex_unlock(&slot->lock);

    bx_rg_sched_note_work_claimed(sched);
    if (stolen_out)
        *stolen_out = false;
    return work;
}

static struct bx_rg_sched_work *bx_rg_sched_try_steal_work(struct bx_rg_sched_state *sched,
                                                           size_t worker_index,
                                                           bool *stolen_out) {
    if (!sched || !sched->worker_slots || sched->thread_count <= 1u)
        return NULL;

    for (size_t offset = 1u; offset < sched->thread_count; ++offset) {
        size_t victim_index = (worker_index + offset) % sched->thread_count;
        struct bx_rg_sched_worker_slot *slot = &sched->worker_slots[victim_index];

        bx_rg_sched_lock_worker_slot(slot);
        if (slot->len == 0u) {
            pthread_mutex_unlock(&slot->lock);
            continue;
        }

        struct bx_rg_sched_work *work = slot->items[0];
        if (slot->len > 1u) {
            memmove(slot->items,
                    slot->items + 1u,
                    (slot->len - 1u) * sizeof(*slot->items));
        }
        slot->len--;
        slot->items[slot->len] = NULL;
        pthread_mutex_unlock(&slot->lock);

        if (work->kind == BX_RG_SCHED_WORK_DIR)
            bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_SUBTREES_STOLEN,
                                                 1u);
        bx_rg_sched_note_work_claimed(sched);
        if (stolen_out)
            *stolen_out = true;
        return work;
    }

    return NULL;
}

static void bx_rg_sched_finish_work(struct bx_rg_sched_state *sched) {
    if (!sched)
        return;

    bx_rg_sched_lock_global(sched);
    if (sched->active_workers > 0u)
        sched->active_workers--;
    if (sched->fatal_error ||
        (sched->pending_work == 0u && sched->active_workers == 0u && sched->idle_workers > 0u)) {
        if (sched->pending_work == 0u && sched->active_workers == 0u)
            sched->shutdown = true;
        bx_rg_sched_signal_workers_locked(sched);
    }
    pthread_mutex_unlock(&sched->lock);
}

static struct bx_rg_sched_work *bx_rg_sched_work_new_dir(const char *path,
                                                         const char *current_root,
                                                         int base_depth,
                                                         bool strip_dot_prefix,
                                                         DIR *owned_dir,
                                                         bool donated_dir,
                                                         bool git_root_resolved,
                                                         bool gitignore_enabled,
                                                         const char *git_root,
                                                         const struct bx_ignore_state
                                                             *parent_ignore_state) {
    struct bx_rg_sched_work *work = calloc(1u, sizeof(*work));
    if (!work) {
        if (owned_dir)
            closedir(owned_dir);
        return NULL;
    }
    work->kind = BX_RG_SCHED_WORK_DIR;
    work->base_depth = base_depth;
    work->strip_dot_prefix = strip_dot_prefix;
    work->u.dir.owned_dir = owned_dir;
    work->u.dir.donated_dir = donated_dir;
    work->u.dir.path_offset = SIZE_MAX;
    work->u.dir.git_root_offset = SIZE_MAX;
    if (!bx_rg_sched_work_store_string(work, path, &work->u.dir.path_offset) ||
        (git_root &&
         !bx_rg_sched_work_store_string(work, git_root, &work->u.dir.git_root_offset))) {
        bx_rg_sched_free_work(work);
        return NULL;
    }
    work->u.dir.git_root_resolved = git_root_resolved;
    work->u.dir.gitignore_enabled = gitignore_enabled;
    if (parent_ignore_state) {
        work->u.dir.parent_ignore_state_snapshot =
            bx_ignore_state_clone_chain_for_subtree(parent_ignore_state, current_root, path);
        if (!work->u.dir.parent_ignore_state_snapshot) {
            bx_rg_sched_free_work(work);
            return NULL;
        }
    }
    return work;
}

static struct bx_rg_sched_work *bx_rg_sched_work_new_batch(void) {
    struct bx_rg_sched_work *work = calloc(1u, sizeof(*work));
    if (!work)
        return NULL;
    work->debug_id = bx_search_dev_batch_debug_next_id();
    bx_search_dev_batch_debug_search("rg_sched", "alloc", work->debug_id, 0u, 0u);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_ALLOCS, 1u);
    work->kind = BX_RG_SCHED_WORK_FILE_BATCH;
    return work;
}

static bool bx_rg_sched_batch_add(struct bx_rg_sched_frontier_state *state,
                                  const char *path,
                                  int base_depth) {
    if (!state)
        return false;
    if (!state->pending_batch) {
        state->pending_batch = bx_rg_sched_work_new_batch();
        if (!state->pending_batch)
            return false;
    }

    struct bx_rg_sched_work *work = state->pending_batch;
    if (work->u.batch.count >= BX_RG_SCHED_BATCH_MAX_FILES)
        return false;

    struct bx_rg_sched_file_item *item = &work->u.batch.items[work->u.batch.count];
    /*
     * File batches are seeded only from explicit argv operands. Borrow those
     * stable strings instead of copying paths into a global search batch
     * before any match or diagnostic publication exists.
     */
    item->path = path;
    item->base_depth = base_depth;
    item->strip_dot_prefix = state->strip_dot_prefix;
    work->u.batch.count++;

    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_FILES_SEEN, 1u);
    return true;
}

static bool bx_rg_sched_flush_pending_batch(struct bx_rg_sched_frontier_state *state) {
    if (!state || !state->pending_batch)
        return true;
    struct bx_rg_sched_work *work = state->pending_batch;
    state->pending_batch = NULL;
    if (work->u.batch.count == 0u) {
        bx_rg_sched_free_work(work);
        return true;
    }
    if (!bx_rg_sched_work_vec_push(state->vec, work)) {
        bx_rg_sched_free_work(work);
        return false;
    }
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_FILES,
                                         (uint64_t)work->u.batch.count);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCH_PATH_BYTES,
                                         (uint64_t)work->storage_len);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_QUEUED, 1u);
    bx_search_dev_batch_debug_search("rg_sched",
                                     "queued",
                                     work->debug_id,
                                     (uint64_t)work->u.batch.count,
                                     (uint64_t)work->storage_len);
    return true;
}

static bool bx_rg_sched_add_root_dir_work(struct bx_rg_sched_work_vec *vec,
                                          const char *path,
                                          bool strip_dot_prefix) {
    struct bx_rg_sched_work *work = bx_rg_sched_work_new_dir(path, path, 0,
                                                             strip_dot_prefix,
                                                             NULL,
                                                             false,
                                                             false, false,
                                                             NULL, NULL);
    if (!work)
        return false;
    if (!bx_rg_sched_work_vec_push(vec, work)) {
        bx_rg_sched_free_work(work);
        return false;
    }
    return true;
}

static const char *bx_rg_sched_display_path(struct bx_rg_sched_worker *worker,
                                            const char *path,
                                            bool strip_dot_prefix,
                                            const struct search_opts *opts) {
    if (!worker || !path)
        return path;
    const char *display = bx_rg_display_path_buf_format(&worker->display_path_buf,
                                                        path,
                                                        strip_dot_prefix,
                                                        opts ? opts->path_separator : '/');
    return display ? display : path;
}

static bool bx_rg_sched_worker_out_reserve(struct bx_rg_sched_worker *worker,
                                           size_t needed) {
    if (!worker)
        return false;
    if (worker->stdout_cap >= needed)
        return true;
    size_t new_cap = worker->stdout_cap == 0u ? 256u : worker->stdout_cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }
    char *tmp = realloc(worker->stdout_buf, new_cap);
    if (!tmp)
        return false;
    worker->stdout_buf = tmp;
    worker->stdout_cap = new_cap;
    return true;
}

static bool bx_rg_sched_append_buf(char **buf,
                                   size_t *len,
                                   size_t *cap,
                                   const char *text,
                                   size_t text_len) {
    if (!buf || !len || !cap || (!text && text_len != 0u))
        return false;
    size_t needed = *len + text_len + 1u;
    if (*cap < needed) {
        size_t new_cap = *cap == 0u ? 128u : *cap;
        while (new_cap < needed) {
            if (new_cap > (SIZE_MAX / 2u))
                return false;
            new_cap *= 2u;
        }
        char *tmp = realloc(*buf, new_cap);
        if (!tmp)
            return false;
        *buf = tmp;
        *cap = new_cap;
    }
    if (text_len > 0u)
        memcpy(*buf + *len, text, text_len);
    *len += text_len;
    (*buf)[*len] = '\0';
    return true;
}

static bool bx_rg_sched_worker_append_output(struct bx_rg_sched_state *sched,
                                             struct bx_rg_sched_worker *worker,
                                             const char *display_name) {
    if (!sched || !worker || !display_name)
        return false;

    char *quoted_display_name =
        bx_search_quote_path_metadata_for_terminal(display_name, sched->opts);
    const char *output_name = quoted_display_name ? quoted_display_name : display_name;
    size_t len = strlen(output_name);
    size_t needed = worker->stdout_len + len + 1u;
    if (!bx_rg_sched_worker_out_reserve(worker, needed)) {
        free(quoted_display_name);
        return false;
    }
    memcpy(worker->stdout_buf + worker->stdout_len, output_name, len);
    worker->stdout_len += len;
    worker->stdout_buf[worker->stdout_len++] = sched->opts->null_output ? '\0' : '\n';
    free(quoted_display_name);
    bx_search_dev_counters_note_output_line_emitted();
    return true;
}

static void bx_rg_sched_publish_emit_record(void *user,
                                            struct bx_rg_publish_record *record) {
    struct bx_rg_sched_state *sched = user;
    FILE *stdout_stream = sched && sched->stdout_stream ? sched->stdout_stream : stdout;

    if (!sched || !record)
        return;

    if (record->used_heading && record->stdout_len > 0u &&
        sched->heading_output_started) {
        fputc('\n', stdout_stream);
    }
    if (record->stdout_len > 0u && record->stdout_buf)
        fwrite(record->stdout_buf, 1u, record->stdout_len, stdout_stream);
    if (record->stderr_len > 0u && record->stderr_buf)
        fwrite(record->stderr_buf, 1u, record->stderr_len, stderr);
    if (record->used_heading && record->stdout_len > 0u)
        sched->heading_output_started = true;

    sched->stats.matches += record->stats.matches;
    sched->stats.matched_lines += record->stats.matched_lines;
    sched->stats.files_with_matches += record->stats.files_with_matches;
    sched->stats.files_searched += record->stats.files_searched;
    sched->stats.bytes_printed += record->stats.bytes_printed;
    sched->stats.bytes_searched += record->stats.bytes_searched;

    bx_rg_sched_apply_result_unlocked(sched, record->match_seen, record->error_seen);
}

static int bx_rg_sched_search_one_captured(struct bx_rg_sched_state *sched,
                                           struct bx_rg_sched_worker *worker,
                                           const struct bx_walk_entry *entry,
                                           const char *path,
                                           bool strip_dot_prefix,
                                           char **stderr_buf,
                                           size_t *stderr_len,
                                           size_t *stderr_cap,
                                           bool *match_seen_out) {
    int dummy_matches = 0;
    char *stdout_buf = NULL;
    size_t stdout_len = 0u;
    char *local_stderr_buf = NULL;
    size_t local_stderr_len = 0u;
    struct bx_search_output_ctx output_ctx = {
        .capture_out_buf = &stdout_buf,
        .capture_out_len = &stdout_len,
        .capture_err_buf = &local_stderr_buf,
        .capture_err_len = &local_stderr_len,
    };
    struct bx_search_output_ctx *previous_ctx = bx_search_output_ctx_push(&output_ctx);
    int status = entry
        ? bx_search_search_walk_entry_with_display_buffer(entry, NULL, strip_dot_prefix,
                                                          sched->progname, worker->matcher,
                                                          sched->exec_plan, sched->opts,
                                                          &dummy_matches, &worker->scanner,
                                                          &worker->record_stream, NULL,
                                                          &worker->display_path_buf)
        : bx_search_search_file_with_display_buffer(path, NULL, strip_dot_prefix,
                                                    sched->progname, worker->matcher,
                                                    sched->exec_plan, sched->opts,
                                                    &dummy_matches, &worker->scanner,
                                                    &worker->record_stream, NULL,
                                                    &worker->display_path_buf);

    bx_search_output_ctx_pop(previous_ctx);
    bx_search_output_ctx_close_captures(&output_ctx);

    if (output_ctx.capture_failed) {
        free(stdout_buf);
        free(local_stderr_buf);
        bx_rg_sched_set_fatal(sched, "rg: failed to allocate worker output streams\n");
        return 2;
    }
    if (stdout_len > 0u &&
        !bx_rg_sched_append_buf(&worker->stdout_buf, &worker->stdout_len,
                                &worker->stdout_cap, stdout_buf, stdout_len)) {
        free(stdout_buf);
        free(local_stderr_buf);
        bx_rg_sched_set_fatal(sched, "rg: failed to append worker output\n");
        return 2;
    }
    if (local_stderr_len > 0u &&
        !bx_rg_sched_append_buf(stderr_buf, stderr_len, stderr_cap,
                                local_stderr_buf, local_stderr_len)) {
        free(stdout_buf);
        free(local_stderr_buf);
        bx_rg_sched_set_fatal(sched, "rg: failed to append worker diagnostics\n");
        return 2;
    }
    free(stdout_buf);
    free(local_stderr_buf);

    if (status == 0 && match_seen_out)
        *match_seen_out = true;
    return status;
}

static int bx_rg_sched_search_one(struct bx_rg_sched_state *sched,
                                  struct bx_rg_sched_worker *worker,
                                  const struct bx_walk_entry *entry,
                                  const char *path,
                                  bool strip_dot_prefix,
                                  bool stolen,
                                  bool defer_output_capture,
                                  char **stderr_buf,
                                  size_t *stderr_len,
                                  size_t *stderr_cap,
                                  bool *match_seen_out) {
    int dummy_matches = 0;

    if (match_seen_out)
        *match_seen_out = false;

    if (!bx_rg_sched_uses_path_only_output(sched->opts)) {
        if (defer_output_capture) {
            int probe_status = entry
                ? bx_search_search_walk_entry_with_display_buffer(entry, NULL,
                                                                  strip_dot_prefix,
                                                                  sched->progname,
                                                                  worker->matcher,
                                                                  &worker->quiet_exec_plan,
                                                                  &sched->quiet_opts,
                                                                  &dummy_matches,
                                                                  &worker->scanner,
                                                                  &worker->record_stream, NULL,
                                                                  &worker->display_path_buf)
                : bx_search_search_file_with_display_buffer(path, NULL,
                                                            strip_dot_prefix,
                                                            sched->progname,
                                                            worker->matcher,
                                                            &worker->quiet_exec_plan,
                                                            &sched->quiet_opts,
                                                            &dummy_matches,
                                                            &worker->scanner,
                                                            &worker->record_stream, NULL,
                                                            &worker->display_path_buf);
            bx_rg_sched_note_file_search(stolen);
            if (probe_status != 0)
                return probe_status;
            return bx_rg_sched_search_one_captured(sched, worker, entry, path,
                                                   strip_dot_prefix,
                                                   stderr_buf, stderr_len,
                                                   stderr_cap, match_seen_out);
        }

        int status = entry
            ? bx_search_search_walk_entry_with_display_buffer(entry, NULL, strip_dot_prefix,
                                                              sched->progname, worker->matcher,
                                                              sched->exec_plan, sched->opts,
                                                              &dummy_matches, &worker->scanner,
                                                              &worker->record_stream, NULL,
                                                              &worker->display_path_buf)
            : bx_search_search_file_with_display_buffer(path, NULL, strip_dot_prefix,
                                                        sched->progname, worker->matcher,
                                                        sched->exec_plan, sched->opts,
                                                        &dummy_matches, &worker->scanner,
                                                        &worker->record_stream, NULL,
                                                        &worker->display_path_buf);
        bx_rg_sched_note_file_search(stolen);
        if (status == 0 && match_seen_out)
            *match_seen_out = true;
        return status;
    }

    int status = entry
        ? bx_search_search_walk_entry_with_display_buffer(entry, NULL, strip_dot_prefix,
                                                          sched->progname, worker->matcher,
                                                          &worker->quiet_exec_plan,
                                                          &sched->quiet_opts,
                                                          &dummy_matches, &worker->scanner,
                                                          &worker->record_stream, NULL,
                                                          &worker->display_path_buf)
        : bx_search_search_file_with_display_buffer(path, NULL, strip_dot_prefix,
                                                    sched->progname, worker->matcher,
                                                    &worker->quiet_exec_plan,
                                                    &sched->quiet_opts,
                                                    &dummy_matches, &worker->scanner,
                                                    &worker->record_stream, NULL,
                                                    &worker->display_path_buf);
    bx_rg_sched_note_file_search(stolen);

    if (status == 0 && sched->opts->files_with_matches) {
        const char *display_name = bx_rg_sched_display_path(worker, path, strip_dot_prefix,
                                                            sched->opts);
        if (!bx_rg_sched_worker_append_output(sched, worker, display_name)) {
            bx_rg_sched_set_fatal(sched, "rg: failed to append worker output\n");
            return 2;
        }
        if (match_seen_out)
            *match_seen_out = true;
        return 0;
    }
    if (status == 1 && sched->opts->files_without_match) {
        const char *display_name = bx_rg_sched_display_path(worker, path, strip_dot_prefix,
                                                            sched->opts);
        if (!bx_rg_sched_worker_append_output(sched, worker, display_name)) {
            bx_rg_sched_set_fatal(sched, "rg: failed to append worker output\n");
            return 2;
        }
        if (match_seen_out)
            *match_seen_out = true;
        return 0;
    }
    return status;
}

static DIR *bx_rg_sched_open_donated_dir(const struct bx_walk_entry *entry) {
    int parent_fd = AT_FDCWD;
    const char *name = NULL;

    if (!entry || !entry->is_dir)
        return NULL;
    if (entry->metadata_dirfd_valid && entry->metadata_name) {
        parent_fd = entry->metadata_dirfd;
        name = entry->metadata_name;
    } else if (entry->path) {
        name = entry->path;
    }
    if (!name)
        return NULL;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_OPENAT_CALLS, 1u);
    int fd;
    if (parent_fd >= 0 && parent_fd != AT_FDCWD && bx_fd_at_name_is_child(name))
        fd = bx_fd_openat_child(parent_fd, name, O_RDONLY | O_DIRECTORY, 0);
    else
        fd = bx_fd_openat_cloexec(parent_fd, name, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        bx_search_dev_counters_note_rg_sched(
            BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPEN_FAILURES, 1u);
        return NULL;
    }

    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        bx_search_dev_counters_note_rg_sched(
            BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPEN_FAILURES, 1u);
        return NULL;
    }
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OPENED, 1u);
    return dir;
}

static enum bx_walk_action bx_rg_sched_walk_visit(struct bx_walk_entry *entry,
                                                  const struct bx_ignore_state *ignore_state,
                                                  const struct bx_walk_ignore_opts *ignore_opts,
                                                  void *user) {
    struct bx_rg_sched_walk_state *state = user;

    if (!state || !state->sched || !state->worker)
        return BX_WALK_ERROR;
    if (bx_cancel_state_requested(&state->sched->cancel))
        return BX_WALK_STOP;
    if (entry->is_dir) {
        if (entry->depth > 0) {
            int global_depth = state->base_depth + entry->depth;
            bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_DIRS_SEEN, 1u);
            bx_rg_sched_note_dir_walk(state->stolen);
            if (entry->depth == 1 &&
                (state->sched->opts->max_depth < 0 || global_depth < state->sched->opts->max_depth) &&
                bx_rg_sched_has_idle_worker(state->sched)) {
                DIR *donated_dir = bx_rg_sched_open_donated_dir(entry);
                if (!donated_dir)
                    return BX_WALK_CONTINUE;
                struct bx_rg_sched_work *work =
                    bx_rg_sched_work_new_dir(entry->path,
                                             state->work_root_path,
                                             global_depth,
                                             state->strip_dot_prefix,
                                             donated_dir,
                                             true,
                                             ignore_opts ? ignore_opts->git_root_resolved : false,
                                             ignore_opts ? ignore_opts->gitignore_enabled : false,
                                             ignore_opts ? ignore_opts->git_root : NULL,
                                             ignore_state);
                if (!work) {
                    bx_rg_sched_set_fatal(state->sched, "rg: failed to queue local subtree work\n");
                    return BX_WALK_ERROR;
                }
                bool donated = false;
                if (!bx_rg_sched_enqueue_local_work_if_idle(state->sched,
                                                            state->worker_index,
                                                            work,
                                                            &donated)) {
                    bx_rg_sched_free_work(work);
                    bx_rg_sched_set_fatal(state->sched, "rg: failed to queue local subtree work\n");
                    return BX_WALK_ERROR;
                }
                if (!donated) {
                    bx_rg_sched_free_work(work);
                    return BX_WALK_CONTINUE;
                }
                return BX_WALK_PRUNE;
            }
        }
        return BX_WALK_CONTINUE;
    }
    if (bx_search_entry_can_skip_max_filesize_zero_literal(entry, state->sched->exec_plan,
                                                           state->sched->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_recursive_special_input(entry, state->sched->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state->sched->opts))
        return BX_WALK_CONTINUE;

    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_FILES_SEEN, 1u);
    bool matched = false;
    int status = bx_rg_sched_search_one(state->sched, state->worker, entry, entry->path,
                                        state->strip_dot_prefix, state->stolen,
                                        state->defer_output_capture,
                                        state->stderr_buf, state->stderr_len,
                                        state->stderr_cap,
                                        &matched);
    if (matched && state->match_seen)
        *state->match_seen = true;
    if (status == 2 && state->error_seen)
        *state->error_seen = true;
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_rg_sched_walk_error(const char *path,
                                                  int errnum,
                                                  void *user) {
    struct bx_rg_sched_walk_state *state = user;

    if (!state || !state->sched)
        return BX_WALK_ERROR;
    if (state->sched->opts && state->sched->opts->suppress_errors) {
        if (state->error_seen)
            *state->error_seen = true;
        return BX_WALK_CONTINUE;
    }
    char msg[4096];
    int n;
    n = bx_search_snprintf_path_error(msg, sizeof(msg),
                                      state->sched->progname, path, errnum);
    if (n < 0 || (size_t)n >= sizeof(msg) ||
        !bx_rg_sched_append_buf(state->stderr_buf, state->stderr_len, state->stderr_cap,
                                msg, (size_t)n)) {
        bx_rg_sched_set_fatal(state->sched, "rg: failed to buffer traversal error\n");
        return BX_WALK_ERROR;
    }
    return BX_WALK_CONTINUE;
}

static struct bx_rg_sched_worker *bx_rg_sched_worker_init(struct bx_rg_sched_state *sched,
                                                          size_t worker_index) {
    (void)worker_index;
    struct bx_rg_sched_worker *worker = calloc(1u, sizeof(*worker));
    if (!worker)
        return NULL;

    char *errmsg = NULL;
    worker->matcher = bx_search_compile_matcher(sched->pattern,
                                                sched->personality,
                                                sched->opts,
                                                &errmsg,
                                                NULL);
    if (!worker->matcher) {
        bx_rg_sched_set_fatal(sched, errmsg ? errmsg : "rg: failed to compile matcher\n");
        free(errmsg);
        free(worker);
        return NULL;
    }
    /*
     * The subtree scheduler publishes presence-only path names itself. Run
     * each per-file -l/-L search under a quiet execution plan so exact-literal
     * presence stays on the absence/raw path instead of entering the
     * line-oriented scanner just to rediscover whether a file matched.
     */
    bx_search_exec_plan_build(&worker->quiet_exec_plan, &sched->quiet_plan,
                              worker->matcher, &sched->quiet_opts);
    return worker;
}

static void bx_rg_sched_worker_fini(struct bx_rg_sched_worker *worker) {
    if (!worker)
        return;
    bx_search_matcher_free(worker->matcher);
    bx_search_scanner_dispose(&worker->scanner);
    bx_record_stream_dispose(&worker->record_stream);
    bx_rg_display_path_buf_dispose(&worker->display_path_buf);
    free(worker->stdout_buf);
    free(worker);
}

static void bx_rg_sched_process_work(struct bx_rg_sched_state *sched,
                                     struct bx_rg_sched_worker *worker,
                                     struct bx_rg_sched_work *work,
                                     size_t worker_index,
                                     bool stolen) {
    struct bx_search_output_ctx output_ctx = {0};
    struct bx_search_output_ctx *previous_ctx = NULL;
    char *walk_stderr_buf = NULL;
    size_t walk_stderr_len = 0u;
    size_t walk_stderr_cap = 0u;
    size_t stderr_cap = 0u;
    size_t stdout_before = worker ? worker->stdout_len : 0u;
    bool job_match_seen = false;
    bool job_error_seen = false;
    bool defer_output_capture = false;
    bool capture_during_work = false;
    bool output_ctx_pushed = false;

    if (!sched || !worker || !work)
        return;

    char *stderr_buf = NULL;
    size_t stderr_len = 0u;
    defer_output_capture = bx_rg_sched_can_defer_output_capture(sched);
    capture_during_work = !sched->opts->suppress_errors ||
                          (!bx_rg_sched_uses_path_only_output(sched->opts) &&
                           !defer_output_capture);
    if (capture_during_work && !bx_rg_sched_uses_path_only_output(sched->opts)) {
        output_ctx.capture_out_buf = &worker->stdout_buf;
        output_ctx.capture_out_len = &worker->stdout_len;
    }
    if (capture_during_work) {
        output_ctx.capture_err_buf = &stderr_buf;
        output_ctx.capture_err_len = &stderr_len;
        previous_ctx = bx_search_output_ctx_push(&output_ctx);
        output_ctx_pushed = true;
    }

    if (work->kind == BX_RG_SCHED_WORK_DIR) {
        const char *work_root_path = bx_rg_sched_work_storage_string(work,
                                                                     work->u.dir.path_offset);
        const char *git_root = bx_rg_sched_work_storage_string(work, work->u.dir.git_root_offset);

        if (!work_root_path) {
            if (output_ctx_pushed)
                bx_search_output_ctx_pop(previous_ctx);
            bx_search_output_ctx_close_captures(&output_ctx);
            free(stderr_buf);
            bx_rg_sched_set_fatal(sched, "rg: failed to resolve queued subtree path\n");
            bx_rg_sched_free_work(work);
            bx_rg_sched_finish_work(sched);
            return;
        }
        if (work->u.dir.git_root_offset != SIZE_MAX && !git_root) {
            if (output_ctx_pushed)
                bx_search_output_ctx_pop(previous_ctx);
            bx_search_output_ctx_close_captures(&output_ctx);
            free(stderr_buf);
            bx_rg_sched_set_fatal(sched, "rg: failed to resolve queued ignore root\n");
            bx_rg_sched_free_work(work);
            bx_rg_sched_finish_work(sched);
            return;
        }
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_DIRS_SEEN, 1u);
        bx_rg_sched_note_dir_walk(stolen);
        struct bx_rg_sched_walk_state walk_state = {
            .sched = sched,
            .worker = worker,
            .stderr_buf = &walk_stderr_buf,
            .stderr_len = &walk_stderr_len,
            .stderr_cap = &walk_stderr_cap,
            .match_seen = &job_match_seen,
            .error_seen = &job_error_seen,
            .worker_index = worker_index,
            .base_depth = work->base_depth,
            .work_root_path = work_root_path,
            .strip_dot_prefix = work->strip_dot_prefix,
            .stolen = stolen,
            .defer_output_capture = defer_output_capture,
        };
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(sched->progname,
                                                                 sched->personality,
                                                                 sched->opts,
                                                                 NULL);
        int relative_max_depth = -1;
        if (sched->opts->max_depth >= 0) {
            relative_max_depth = sched->opts->max_depth > work->base_depth
                ? sched->opts->max_depth - work->base_depth
                : 0;
        }
        walk_opts.max_depth = relative_max_depth;
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(sched->opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(sched->progname,
                                                                            sched->opts);
        ignore_opts.git_root = git_root;
        ignore_opts.git_root_resolved = work->u.dir.git_root_resolved;
        ignore_opts.gitignore_enabled = work->u.dir.gitignore_enabled;
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit_with_ignore = bx_rg_sched_walk_visit,
            .error = bx_rg_sched_walk_error,
            .borrowed_parent_ignore_state = work->u.dir.parent_ignore_state_snapshot,
        };
        DIR *owned_dir = work->u.dir.owned_dir;
        work->u.dir.owned_dir = NULL;
        if (work->u.dir.donated_dir) {
            bx_search_dev_counters_note_rg_sched(
                owned_dir ? BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_OWNED_WALKS
                          : BX_SEARCH_RG_SCHED_WORKER_DONATED_DIR_REOPEN_FALLBACKS,
                1u);
        }
        int rc = owned_dir
            ? bx_search_walk_opened_dir(work_root_path, owned_dir, &walk_config, &walk_state)
            : bx_search_walk(work_root_path, &walk_config, &walk_state);
        if (rc != 0)
            job_error_seen = true;
    } else {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_SEARCHED, 1u);
        bx_search_dev_batch_debug_search("rg_sched",
                                         "searched",
                                         work->debug_id,
                                         (uint64_t)work->u.batch.count,
                                         (uint64_t)work->storage_len);
        for (size_t i = 0; i < work->u.batch.count; ++i) {
            const char *path = work->u.batch.items[i].path;
            if (!path) {
                bx_rg_sched_set_fatal(sched, "rg: failed to resolve queued batch path\n");
                job_error_seen = true;
                break;
            }
            bool matched = false;
            int status = bx_rg_sched_search_one(sched,
                                                worker,
                                                NULL,
                                                path,
                                                work->u.batch.items[i].strip_dot_prefix,
                                                stolen,
                                                defer_output_capture,
                                                &stderr_buf,
                                                &stderr_len,
                                                &stderr_cap,
                                                &matched);
            if (status == 2)
                job_error_seen = true;
            if (matched)
                job_match_seen = true;
            if (bx_cancel_state_requested(&sched->cancel))
                break;
        }
    }

    if (output_ctx_pushed)
        bx_search_output_ctx_pop(previous_ctx);
    bx_search_output_ctx_close_captures(&output_ctx);
    if (capture_during_work)
        stderr_cap = stderr_len > 0u ? stderr_len + 1u : 0u;

    if (output_ctx.capture_failed) {
        free(stderr_buf);
        free(walk_stderr_buf);
        bx_rg_sched_set_fatal(sched, "rg: failed to allocate worker output streams\n");
        bx_rg_sched_free_work(work);
        bx_rg_sched_finish_work(sched);
        return;
    }
    if (walk_stderr_len > 0u) {
        if (!bx_rg_sched_append_buf(&stderr_buf, &stderr_len, &stderr_cap,
                                    walk_stderr_buf, walk_stderr_len)) {
            free(stderr_buf);
            free(walk_stderr_buf);
            bx_rg_sched_set_fatal(sched, "rg: failed to merge traversal errors\n");
            bx_rg_sched_free_work(work);
            bx_rg_sched_finish_work(sched);
            return;
        }
    }
    free(walk_stderr_buf);
    if (worker->stdout_len > stdout_before)
        job_match_seen = true;

    if (worker->stdout_len == 0u && stderr_len == 0u &&
        work->kind == BX_RG_SCHED_WORK_FILE_BATCH) {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_SEARCH_BATCHES_EMPTY, 1u);
        bx_search_dev_batch_debug_search("rg_sched",
                                         "empty-result",
                                         work->debug_id,
                                         (uint64_t)work->u.batch.count,
                                         (uint64_t)work->storage_len);
    }

    if (worker->stdout_len == 0u && stderr_len == 0u) {
        free(stderr_buf);
        /*
         * Avoid queuing an output batch unless this work actually emitted at
         * least one path or diagnostic. No-output results merge directly.
         */
        bx_rg_sched_merge_direct_result(sched, job_match_seen, job_error_seen);
        bx_rg_sched_free_work(work);
        bx_rg_sched_finish_work(sched);
        return;
    }

    struct bx_rg_publish_record *record = calloc(1u, sizeof(*record));
    if (!record) {
        free(stderr_buf);
        bx_rg_sched_set_fatal(sched, "rg: failed to allocate worker output record\n");
        bx_rg_sched_free_work(work);
        bx_rg_sched_finish_work(sched);
        return;
    }
    record->stdout_buf = worker->stdout_buf;
    record->stdout_len = worker->stdout_len;
    record->stderr_buf = stderr_buf;
    record->stderr_len = stderr_len;
    record->match_seen = job_match_seen;
    record->error_seen = job_error_seen;
    record->used_heading = output_ctx.used_heading;
    worker->stdout_buf = NULL;
    worker->stdout_len = 0u;
    worker->stdout_cap = 0u;
    if (!bx_rg_publish_submit(&sched->publish, record)) {
        bx_rg_publish_dispose_record(record);
        bx_rg_sched_set_fatal(sched, "rg: failed to publish worker output\n");
    }

    bx_rg_sched_free_work(work);
    bx_rg_sched_finish_work(sched);
}

static void *bx_rg_sched_worker_main(void *arg) {
    struct bx_rg_sched_thread_arg *thread_arg = arg;
    struct bx_rg_sched_state *sched = thread_arg ? thread_arg->sched : NULL;
    size_t worker_index = thread_arg ? thread_arg->worker_index : 0u;
    struct bx_rg_sched_worker *worker;

    if (!sched)
        return NULL;

    worker = bx_rg_sched_worker_init(sched, worker_index);
    if (!worker)
        return NULL;

    for (;;) {
        if (bx_cancel_state_requested(&sched->cancel))
            break;

        bool stolen = false;
        struct bx_rg_sched_work *work = bx_rg_sched_try_pop_local_work(sched, worker_index, &stolen);
        if (!work)
            work = bx_rg_sched_try_steal_work(sched, worker_index, &stolen);
        if (work) {
            bx_rg_sched_process_work(sched, worker, work, worker_index, stolen);
            continue;
        }

        bx_rg_sched_lock_global(sched);
        if (sched->fatal_error || sched->shutdown || bx_cancel_state_requested(&sched->cancel)) {
            pthread_mutex_unlock(&sched->lock);
            break;
        }
        if (sched->pending_work > 0u) {
            pthread_mutex_unlock(&sched->lock);
            continue;
        }
        if (sched->active_workers == 0u) {
            sched->shutdown = true;
            bx_rg_sched_signal_workers_locked(sched);
            pthread_mutex_unlock(&sched->lock);
            break;
        }

        sched->idle_workers++;
        while (!sched->fatal_error &&
               !sched->shutdown &&
               !bx_cancel_state_requested(&sched->cancel) &&
               sched->pending_work == 0u &&
               sched->active_workers > 0u) {
            pthread_cond_wait(&sched->work_ready, &sched->lock);
        }
        sched->idle_workers--;

        bool should_exit = sched->fatal_error ||
                           sched->shutdown ||
                           bx_cancel_state_requested(&sched->cancel);
        if (!should_exit && sched->pending_work == 0u && sched->active_workers == 0u) {
            sched->shutdown = true;
            bx_rg_sched_signal_workers_locked(sched);
            should_exit = true;
        }
        pthread_mutex_unlock(&sched->lock);
        if (should_exit)
            break;
    }

    bx_rg_sched_worker_fini(worker);
    return NULL;
}

static bool bx_rg_sched_seed_frontier(struct bx_rg_sched_state *sched,
                                      struct bx_rg_sched_work_vec *frontier) {
    if (!sched || !frontier)
        return false;

    for (size_t i = 0; i < frontier->len; ++i) {
        struct bx_rg_sched_work *work = frontier->items[i];
        if (!work)
            continue;
        size_t worker_index = sched->thread_count == 0u ? 0u : (i % sched->thread_count);
        if (!bx_rg_sched_enqueue_local_work(sched, worker_index, work))
            return false;
        frontier->items[i] = NULL;
    }
    return true;
}

static void bx_rg_sched_work_vec_dispose(struct bx_rg_sched_work_vec *vec) {
    if (!vec)
        return;
    for (size_t i = 0; i < vec->len; ++i)
        bx_rg_sched_free_work(vec->items[i]);
    free(vec->items);
    vec->items = NULL;
    vec->len = 0u;
    vec->cap = 0u;
}

static bool bx_rg_sched_pattern_is_subtree_filter_exact(const char *pattern) {
    return !pattern || strchr(pattern, '/') == NULL;
}

static bool bx_rg_sched_patterns_are_subtree_filter_exact(char *const *patterns,
                                                          int pattern_count) {
    for (int i = 0; i < pattern_count; ++i) {
        if (!bx_rg_sched_pattern_is_subtree_filter_exact(patterns[i]))
            return false;
    }
    return true;
}

static bool bx_rg_sched_filter_state_exact_for_subtree(const struct search_opts *opts) {
    if (!opts)
        return false;
    /*
     * CLI include/exclude globs are evaluated relative to the original search
     * root when they contain a path separator. Subtree workers initialize
     * filter state from the donated subtree root, so slash-containing patterns
     * would be rebased incorrectly. Basename-only globs and exclude-dir globs
     * are name-only checks and remain exact under subtree donation.
     */
    return bx_rg_sched_patterns_are_subtree_filter_exact(opts->include_patterns,
                                                         opts->num_include) &&
           bx_rg_sched_patterns_are_subtree_filter_exact(opts->exclude_patterns,
                                                         opts->num_exclude);
}

bool bx_rg_sched_supported(enum bx_search_personality personality,
                           const struct search_opts *opts,
                           int num_files,
                           bool rg_searches_stdin) {
    if (!opts || personality != BX_SEARCH_RG)
        return false;
    if (opts->files_only || opts->trace || opts->quiet || opts->stats || rg_searches_stdin)
        return false;
    if (bx_search_sort_requested(opts))
        return false;
    if (!bx_rg_sched_filter_state_exact_for_subtree(opts))
        return false;
    if (!bx_rg_sched_uses_path_only_output(opts)) {
        if (opts->count_only || opts->count_matches)
            return false;
        if (opts->multiline || opts->invert_match)
            return false;
        if (opts->pre_command || opts->search_zip ||
            opts->encoding_mode == BX_RG_ENCODING_EXPLICIT) {
            return false;
        }
        if (bx_search_plan_needs_line_buffering(opts))
            return false;
        if (opts->heading)
            return false;
        if (opts->replace || opts->only_matching || opts->passthru || opts->vimgrep)
            return false;
        if (opts->stop_on_nonmatch)
            return false;
    }
    if (bx_thread_count_resolve(opts->threads) <= 1u)
        return false;
    if (num_files == 0)
        return true;
    return opts->recursive;
}

int bx_rg_sched_run(int argc,
                    char **argv,
                    int first_file,
                    struct bx_search_operand_ref *sorted_operands,
                    int sorted_operand_count,
                    const char *progname,
                    const char *pattern,
                    enum bx_search_personality personality,
                    const struct bx_search_exec_plan *exec_plan,
                    struct search_opts *opts,
                    size_t thread_count,
                    struct bx_search_stats *stats_out,
                    bool *match_seen_out,
                    bool *error_seen_out) {
    struct bx_rg_sched_state sched = {
        .progname = progname,
        .pattern = pattern,
        .personality = personality,
        .exec_plan = exec_plan,
        .opts = opts,
        .exit_status = 1,
        .stdout_stream = bx_search_output_stream(),
        .thread_count = thread_count,
    };
    struct bx_rg_sched_work_vec frontier = {0};
    pthread_t *threads = NULL;
    struct bx_rg_sched_thread_arg *thread_args = NULL;
    size_t started_threads = 0u;

    sched.quiet_opts = *opts;
    sched.quiet_opts.quiet = true;
    sched.quiet_opts.files_with_matches = false;
    sched.quiet_opts.files_without_match = false;
    sched.quiet_opts.stats = false;
    bx_search_plan_build(&sched.quiet_plan, personality, &sched.quiet_opts, 1, false);
    bx_cancel_state_init(&sched.cancel);
    if (pthread_mutex_init(&sched.lock, NULL) != 0)
        return 2;
    if (pthread_cond_init(&sched.work_ready, NULL) != 0) {
        pthread_mutex_destroy(&sched.lock);
        return 2;
    }

    int num_files = argc - first_file;
    if (num_files == 0) {
        if (!bx_rg_sched_add_root_dir_work(&frontier, ".", true))
            bx_rg_sched_set_fatal(&sched, "rg: failed to queue recursive root\n");
    } else {
        for (int operand_i = 0; operand_i < num_files; ++operand_i) {
            int j = sorted_operands
                        ? sorted_operands[bx_search_sort_is_descending(opts)
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            struct stat st;
            bx_search_dev_counters_note_walk_stat_call(BX_SEARCH_WALK_STAT_REASON_EXPLICIT_OPERAND);
            if (stat(argv[j], &st) != 0) {
                bx_rg_sched_report_path_error(&sched, argv[j], errno, num_files == 1);
                bx_rg_sched_apply_result_unlocked(&sched, false, true);
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (!bx_rg_sched_add_root_dir_work(&frontier, argv[j], false))
                    bx_rg_sched_set_fatal(&sched, "rg: failed to queue recursive root\n");
                continue;
            }
            if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                continue;
            if (!bx_search_explicit_entry_selected(opts, argv[j]))
                continue;
            if (bx_search_mode_can_skip_max_filesize_zero_literal(st.st_mode,
                                                                  sched.exec_plan,
                                                                  opts))
                continue;
            if (bx_search_loaded_metadata_exceeds_max_filesize(&st, opts))
                continue;
            struct bx_rg_sched_frontier_state batch_state = {
                .sched = &sched,
                .vec = &frontier,
                .strip_dot_prefix = false,
            };
            if (!bx_rg_sched_batch_add(&batch_state, argv[j], 0) ||
                !bx_rg_sched_flush_pending_batch(&batch_state)) {
                bx_rg_sched_set_fatal(&sched, "rg: failed to queue root file batch\n");
                break;
            }
        }
    }

    if (!sched.fatal_error && frontier.len > 0u && !bx_rg_sched_worker_slots_init(&sched))
        bx_rg_sched_set_fatal(&sched, "rg: failed to initialize worker-local rg scheduler\n");

    if (!sched.fatal_error && frontier.len > 0u && !bx_rg_sched_seed_frontier(&sched, &frontier))
        bx_rg_sched_set_fatal(&sched, "rg: failed to seed subtree scheduler\n");

    if (!sched.fatal_error && frontier.len > 0u &&
        !bx_rg_publish_init(&sched.publish, &(struct bx_rg_publish_opts){
            .mode = BX_RG_PUBLISH_UNORDERED,
            .max_pending = sched.thread_count > 0u ? sched.thread_count : 1u,
            .first_seq = 0u,
            .user = &sched,
            .record_seq = NULL,
            .emit_record = bx_rg_sched_publish_emit_record,
            .dispose_record = bx_rg_sched_publish_dispose_record,
        })) {
        bx_rg_sched_set_fatal(&sched, "rg: failed to initialize unordered publication\n");
    } else if (!sched.fatal_error && frontier.len > 0u) {
        sched.publish_ready = true;
    }

    if (!sched.fatal_error && frontier.len > 0u) {
        threads = calloc(sched.thread_count, sizeof(*threads));
        thread_args = calloc(sched.thread_count, sizeof(*thread_args));
        if (!threads || !thread_args) {
            bx_rg_sched_set_fatal(&sched, "rg: failed to allocate subtree worker threads\n");
        } else {
            for (size_t i = 0; i < sched.thread_count; ++i) {
                thread_args[i] = (struct bx_rg_sched_thread_arg){
                    .sched = &sched,
                    .worker_index = i,
                };
                if (pthread_create(&threads[i], NULL, bx_rg_sched_worker_main, &thread_args[i]) != 0) {
                    bx_rg_sched_set_fatal(&sched, "rg: failed to start subtree worker thread\n");
                    break;
                }
                started_threads++;
            }
        }
    }

    bool join_ok = true;
    for (size_t i = 0; i < started_threads; ++i) {
        if (pthread_join(threads[i], NULL) != 0)
            join_ok = false;
    }
    if (started_threads > 0u && !join_ok && !sched.fatal_error)
        bx_rg_sched_set_fatal(&sched, "rg: subtree worker threads failed\n");

    if (sched.publish_ready) {
        bx_rg_publish_close(&sched.publish);
        bx_rg_publish_join(&sched.publish);
    }
    if (sched.fatal_error) {
        sched.error_seen = true;
        sched.exit_status = 2;
        if (sched.fatal_message && *sched.fatal_message) {
            fputs(sched.fatal_message, stderr);
            if (sched.fatal_message[strlen(sched.fatal_message) - 1] != '\n')
                fputc('\n', stderr);
        }
    }

    if (stats_out)
        *stats_out = sched.stats;
    if (match_seen_out)
        *match_seen_out = sched.match_seen;
    if (error_seen_out)
        *error_seen_out = sched.error_seen;

    if (sched.publish_ready)
        bx_rg_publish_dispose(&sched.publish);
    bx_rg_sched_work_vec_dispose(&frontier);
    bx_rg_sched_worker_slots_dispose(&sched);
    free(thread_args);
    free(threads);
    pthread_cond_destroy(&sched.work_ready);
    pthread_mutex_destroy(&sched.lock);
    free(sched.fatal_message);
    return sched.exit_status;
}
