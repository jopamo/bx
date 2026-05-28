#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <pthread.h>

#include "dev_counters.h"
#include "lib/cancel_state.h"
#include "lib/thread_count.h"
#include "record_stream.h"
#include "rg_parallel.h"
#include "rg_publish.h"
#include "rg_sched.h"
#include "scanner.h"
#include "search_internal.h"
#include "sort.h"
#include "traverse.h"

#define BX_RG_SCHED_BATCH_MAX_FILES 64u

enum bx_rg_sched_work_kind {
    BX_RG_SCHED_WORK_DIR = 0,
    BX_RG_SCHED_WORK_FILE_BATCH,
};

struct bx_rg_sched_file_item {
    char *path;
    int base_depth;
    bool strip_dot_prefix;
};

struct bx_rg_sched_work {
    enum bx_rg_sched_work_kind kind;
    int base_depth;
    bool strip_dot_prefix;
    size_t storage_len;
    size_t storage_cap;
    char *storage;
    union {
        struct {
            char *path;
            char *git_root;
            struct bx_ignore_state *parent_ignore_state;
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
    struct bx_search_stats stats;
    struct bx_rg_publish_aggregate aggregate;
    char *fatal_message;
    size_t thread_count;
    size_t pending_work;
    size_t active_workers;
    size_t idle_workers;
};

struct bx_rg_sched_worker {
    struct bx_matcher *matcher;
    struct bx_search_scanner scanner;
    struct bx_record_stream record_stream;
    char *display_scratch;
    size_t display_scratch_cap;
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
    size_t worker_index;
    int base_depth;
    const char *work_root_path;
    bool strip_dot_prefix;
    bool stolen;
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

static void bx_rg_sched_publish_dispose_record(void *user,
                                               struct bx_rg_publish_record *record) {
    (void)user;
    bx_rg_publish_dispose_record(record);
}

static void bx_rg_sched_signal_workers_locked(struct bx_rg_sched_state *sched) {
    if (!sched)
        return;
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

static void bx_rg_sched_merge_direct_result(struct bx_rg_sched_state *sched,
                                            bool match_seen,
                                            bool error_seen) {
    if (!sched)
        return;

    if (sched->publish_ready)
        pthread_mutex_lock(&sched->publish.unordered_lock);
    if (match_seen) {
        sched->match_seen = true;
        if (sched->exit_status != 2)
            sched->exit_status = 0;
    }
    if (error_seen) {
        sched->error_seen = true;
        sched->exit_status = 2;
    }
    if (sched->publish_ready)
        pthread_mutex_unlock(&sched->publish.unordered_lock);
}

static void bx_rg_sched_set_fatal(struct bx_rg_sched_state *state,
                                  const char *message) {
    if (!state)
        return;

    pthread_mutex_lock(&state->lock);
    state->fatal_error = true;
    state->shutdown = true;
    if (!state->fatal_message && message)
        state->fatal_message = strdup(message);
    bx_rg_sched_signal_workers_locked(state);
    pthread_mutex_unlock(&state->lock);

    bx_cancel_state_request(&state->cancel);
    if (state->publish_ready)
        bx_rg_publish_wake(&state->publish);
}

static void bx_rg_sched_report_path_error(const struct bx_rg_sched_state *state,
                                          const char *path,
                                          int errnum) {
    if (!state || (state->opts && state->opts->suppress_errors))
        return;

    if (bx_search_progname_uses_os_error_style(state->progname))
        fprintf(stderr, "%s: %s: %s (os error %d)\n",
                state->progname, path, strerror(errnum), errnum);
    else
        fprintf(stderr, "%s: %s: %s\n",
                state->progname, path, strerror(errnum));
}

static void bx_rg_sched_free_work(struct bx_rg_sched_work *work) {
    if (!work)
        return;
    if (work->kind == BX_RG_SCHED_WORK_DIR)
        bx_ignore_state_dispose_chain(work->u.dir.parent_ignore_state);
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

    uintptr_t old_storage_addr = (uintptr_t)work->storage;
    char *tmp = realloc(work->storage, new_cap);
    if (!tmp)
        return false;
    if (old_storage_addr != 0u && (uintptr_t)tmp != old_storage_addr) {
        ptrdiff_t delta = (ptrdiff_t)((uintptr_t)tmp - old_storage_addr);
        if (work->kind == BX_RG_SCHED_WORK_DIR) {
            if (work->u.dir.path)
                work->u.dir.path += delta;
            if (work->u.dir.git_root)
                work->u.dir.git_root += delta;
        } else {
            for (size_t i = 0; i < work->u.batch.count; ++i) {
                if (work->u.batch.items[i].path)
                    work->u.batch.items[i].path += delta;
            }
        }
    }
    work->storage = tmp;
    work->storage_cap = new_cap;
    return true;
}

static char *bx_rg_sched_work_store_string(struct bx_rg_sched_work *work,
                                           const char *text) {
    if (!work || !text)
        return NULL;

    size_t len = strlen(text) + 1u;
    if (!bx_rg_sched_work_reserve_storage(work, work->storage_len + len))
        return NULL;

    char *dest = work->storage + work->storage_len;
    memcpy(dest, text, len);
    work->storage_len += len;
    return dest;
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

static bool bx_rg_sched_enqueue_local_work(struct bx_rg_sched_state *sched,
                                           size_t worker_index,
                                           struct bx_rg_sched_work *work) {
    if (!sched || !sched->worker_slots || worker_index >= sched->thread_count || !work)
        return false;

    struct bx_rg_sched_worker_slot *slot = &sched->worker_slots[worker_index];
    pthread_mutex_lock(&slot->lock);
    bool ok = bx_rg_sched_worker_slot_reserve(slot, slot->len + 1u);
    if (ok)
        slot->items[slot->len++] = work;
    pthread_mutex_unlock(&slot->lock);
    if (!ok)
        return false;

    pthread_mutex_lock(&sched->lock);
    sched->pending_work++;
    if (sched->idle_workers > 0u && !sched->shutdown)
        bx_rg_sched_signal_workers_locked(sched);
    pthread_mutex_unlock(&sched->lock);
    return true;
}

static void bx_rg_sched_note_work_claimed(struct bx_rg_sched_state *sched) {
    pthread_mutex_lock(&sched->lock);
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
    pthread_mutex_lock(&slot->lock);
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

        pthread_mutex_lock(&slot->lock);
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

    pthread_mutex_lock(&sched->lock);
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
                                                         const char *git_root,
                                                         const struct bx_ignore_state *parent_ignore_state) {
    struct bx_rg_sched_work *work = calloc(1u, sizeof(*work));
    if (!work)
        return NULL;
    work->kind = BX_RG_SCHED_WORK_DIR;
    work->base_depth = base_depth;
    work->strip_dot_prefix = strip_dot_prefix;
    work->u.dir.path = bx_rg_sched_work_store_string(work, path);
    if (!work->u.dir.path ||
        (git_root && !(work->u.dir.git_root = bx_rg_sched_work_store_string(work, git_root)))) {
        bx_rg_sched_free_work(work);
        return NULL;
    }
    work->u.dir.gitignore_enabled = git_root != NULL;
    if (parent_ignore_state) {
        work->u.dir.parent_ignore_state =
            bx_ignore_state_clone_chain_for_subtree(parent_ignore_state, current_root, path);
        if (!work->u.dir.parent_ignore_state) {
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
    item->path = bx_rg_sched_work_store_string(work, path);
    if (!item->path)
        return false;
    item->base_depth = base_depth;
    item->strip_dot_prefix = state->strip_dot_prefix;
    work->u.batch.count++;

    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_FILES_SEEN, 1u);
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_PATH_BYTES_COPIED,
                                         (uint64_t)(strlen(path) + 1u));
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_PATH_COPIES_BEFORE_MATCH, 1u);
    return true;
}

static bool bx_rg_sched_flush_pending_batch(struct bx_rg_sched_frontier_state *state) {
    if (!state || !state->pending_batch)
        return true;
    struct bx_rg_sched_work *work = state->pending_batch;
    state->pending_batch = NULL;
    if (!bx_rg_sched_work_vec_push(state->vec, work)) {
        bx_rg_sched_free_work(work);
        return false;
    }
    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_BATCHES_BUILT, 1u);
    return true;
}

static enum bx_walk_action bx_rg_sched_frontier_visit(struct bx_walk_entry *entry,
                                                      const struct bx_ignore_state *ignore_state,
                                                      const struct bx_walk_ignore_opts *ignore_opts,
                                                      void *user) {
    struct bx_rg_sched_frontier_state *state = user;

    if (!state || !state->sched)
        return BX_WALK_ERROR;
    if (entry->is_dir) {
        if (entry->depth == 0)
            return BX_WALK_CONTINUE;
        if (!bx_rg_sched_flush_pending_batch(state))
            return BX_WALK_ERROR;
        struct bx_rg_sched_work *work = bx_rg_sched_work_new_dir(entry->path,
                                                                 state->root_path,
                                                                 entry->depth,
                                                                 state->strip_dot_prefix,
                                                                 ignore_opts ? ignore_opts->git_root : NULL,
                                                                 ignore_state);
        if (!work)
            return BX_WALK_ERROR;
        if (!bx_rg_sched_work_vec_push(state->vec, work)) {
            bx_rg_sched_free_work(work);
            return BX_WALK_ERROR;
        }
        return BX_WALK_PRUNE;
    }

    if (bx_search_entry_exceeds_max_filesize(entry, state->sched->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_recursive_special_input(entry, state->sched->opts))
        return BX_WALK_CONTINUE;

    if (state->pending_batch && state->pending_batch->u.batch.count >= BX_RG_SCHED_BATCH_MAX_FILES) {
        if (!bx_rg_sched_flush_pending_batch(state))
            return BX_WALK_ERROR;
    }
    if (!bx_rg_sched_batch_add(state, entry->path, entry->depth))
        return BX_WALK_ERROR;
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_rg_sched_frontier_error(const char *path,
                                                      int errnum,
                                                      void *user) {
    struct bx_rg_sched_frontier_state *state = user;
    if (!state || !state->sched)
        return BX_WALK_ERROR;
    bx_rg_sched_report_path_error(state->sched, path, errnum);
    state->sched->error_seen = true;
    state->sched->exit_status = 2;
    return BX_WALK_CONTINUE;
}

static bool bx_rg_sched_worker_display_reserve(struct bx_rg_sched_worker *worker,
                                               size_t needed) {
    if (!worker)
        return false;
    if (worker->display_scratch_cap >= needed)
        return true;
    size_t new_cap = worker->display_scratch_cap == 0u ? 256u : worker->display_scratch_cap;
    while (new_cap < needed) {
        if (new_cap > (SIZE_MAX / 2u))
            return false;
        new_cap *= 2u;
    }
    char *tmp = realloc(worker->display_scratch, new_cap);
    if (!tmp)
        return false;
    worker->display_scratch = tmp;
    worker->display_scratch_cap = new_cap;
    return true;
}

static const char *bx_rg_sched_display_path(struct bx_rg_sched_worker *worker,
                                            const char *path,
                                            bool strip_dot_prefix,
                                            const struct search_opts *opts) {
    const char *display = path;

    if (strip_dot_prefix && path[0] == '.' && path[1] == '/')
        display = path + 2u;
    if (!opts || opts->path_separator == '/')
        return display;

    size_t len = strlen(display);
    if (!bx_rg_sched_worker_display_reserve(worker, len + 1u))
        return display;
    for (size_t i = 0; i < len; ++i) {
        char ch = display[i];
        worker->display_scratch[i] = (ch == '/') ? opts->path_separator : ch;
    }
    worker->display_scratch[len] = '\0';
    return worker->display_scratch;
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

    size_t len = strlen(display_name);
    size_t needed = worker->stdout_len + len + 1u;
    if (!bx_rg_sched_worker_out_reserve(worker, needed))
        return false;
    memcpy(worker->stdout_buf + worker->stdout_len, display_name, len);
    worker->stdout_len += len;
    worker->stdout_buf[worker->stdout_len++] = sched->opts->null_output ? '\0' : '\n';
    bx_search_dev_counters_note_output_line_emitted();
    return true;
}

static int bx_rg_sched_search_one(struct bx_rg_sched_state *sched,
                                  struct bx_rg_sched_worker *worker,
                                  const char *path,
                                  bool strip_dot_prefix,
                                  bool stolen) {
    int dummy_matches = 0;
    int status = bx_search_search_file(path,
                                       NULL,
                                       strip_dot_prefix,
                                       sched->progname,
                                       worker->matcher,
                                       sched->exec_plan,
                                       &sched->quiet_opts,
                                       &dummy_matches,
                                       &worker->scanner,
                                       &worker->record_stream,
                                       NULL);
    bx_rg_sched_note_file_search(stolen);
    if (status == 0 && sched->opts->files_with_matches) {
        const char *display_name = bx_rg_sched_display_path(worker, path, strip_dot_prefix,
                                                            sched->opts);
        if (!bx_rg_sched_worker_append_output(sched, worker, display_name)) {
            bx_rg_sched_set_fatal(sched, "rg: failed to append worker output\n");
            return 2;
        }
        return 0;
    }
    if (status == 1 && sched->opts->files_without_match) {
        const char *display_name = bx_rg_sched_display_path(worker, path, strip_dot_prefix,
                                                            sched->opts);
        if (!bx_rg_sched_worker_append_output(sched, worker, display_name)) {
            bx_rg_sched_set_fatal(sched, "rg: failed to append worker output\n");
            return 2;
        }
        return 0;
    }
    return status;
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
                (state->sched->opts->max_depth < 0 || global_depth < state->sched->opts->max_depth)) {
                struct bx_rg_sched_work *work = bx_rg_sched_work_new_dir(entry->path,
                                                                         state->work_root_path,
                                                                         global_depth,
                                                                         state->strip_dot_prefix,
                                                                         ignore_opts ? ignore_opts->git_root : NULL,
                                                                         ignore_state);
                if (!work) {
                    bx_rg_sched_set_fatal(state->sched, "rg: failed to queue local subtree work\n");
                    return BX_WALK_ERROR;
                }
                if (!bx_rg_sched_enqueue_local_work(state->sched, state->worker_index, work)) {
                    bx_rg_sched_free_work(work);
                    bx_rg_sched_set_fatal(state->sched, "rg: failed to queue local subtree work\n");
                    return BX_WALK_ERROR;
                }
                return BX_WALK_PRUNE;
            }
        }
        return BX_WALK_CONTINUE;
    }
    if (bx_search_entry_exceeds_max_filesize(entry, state->sched->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_recursive_special_input(entry, state->sched->opts))
        return BX_WALK_CONTINUE;

    bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_FILES_SEEN, 1u);
    (void)bx_rg_sched_search_one(state->sched, state->worker, entry->path,
                                 state->strip_dot_prefix, state->stolen);
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_rg_sched_walk_error(const char *path,
                                                  int errnum,
                                                  void *user) {
    struct bx_rg_sched_walk_state *state = user;

    if (!state || !state->sched)
        return BX_WALK_ERROR;
    char msg[4096];
    int n;
    if (bx_search_progname_uses_os_error_style(state->sched->progname))
        n = snprintf(msg, sizeof(msg), "%s: %s: %s (os error %d)\n",
                     state->sched->progname, path, strerror(errnum), errnum);
    else
        n = snprintf(msg, sizeof(msg), "%s: %s: %s\n",
                     state->sched->progname, path, strerror(errnum));
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
    return worker;
}

static void bx_rg_sched_worker_fini(struct bx_rg_sched_worker *worker) {
    if (!worker)
        return;
    bx_search_matcher_free(worker->matcher);
    bx_search_scanner_dispose(&worker->scanner);
    bx_record_stream_dispose(&worker->record_stream);
    free(worker->display_scratch);
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
    bool job_match_seen = false;
    bool job_error_seen = false;

    if (!sched || !worker || !work)
        return;

    char *stderr_buf = NULL;
    size_t stderr_len = 0u;
    output_ctx.capture_err_buf = &stderr_buf;
    output_ctx.capture_err_len = &stderr_len;
    previous_ctx = bx_search_output_ctx_push(&output_ctx);

    if (work->kind == BX_RG_SCHED_WORK_DIR) {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_DIRS_SEEN, 1u);
        bx_rg_sched_note_dir_walk(stolen);
        struct bx_rg_sched_walk_state walk_state = {
            .sched = sched,
            .worker = worker,
            .stderr_buf = &walk_stderr_buf,
            .stderr_len = &walk_stderr_len,
            .stderr_cap = &walk_stderr_cap,
            .worker_index = worker_index,
            .base_depth = work->base_depth,
            .work_root_path = work->u.dir.path,
            .strip_dot_prefix = work->strip_dot_prefix,
            .stolen = stolen,
        };
        struct bx_walk_opts walk_opts = bx_search_make_walk_opts(sched->progname,
                                                                 sched->personality,
                                                                 sched->opts,
                                                                 NULL);
        int relative_max_depth = 1;
        if (sched->opts->max_depth >= 0) {
            relative_max_depth = sched->opts->max_depth - work->base_depth;
            if (relative_max_depth > 1)
                relative_max_depth = 1;
        }
        walk_opts.max_depth = relative_max_depth;
        struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(sched->opts);
        struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(sched->progname,
                                                                            sched->opts);
        if (work->u.dir.git_root) {
            ignore_opts.git_root = work->u.dir.git_root;
            ignore_opts.gitignore_enabled = work->u.dir.gitignore_enabled;
        }
        struct bx_ignore_state *inherited_ignore = work->u.dir.parent_ignore_state;
        work->u.dir.parent_ignore_state = NULL;
        struct bx_search_walk_config walk_config = {
            .walk_opts = &walk_opts,
            .filter_opts = &filter_opts,
            .ignore_opts = &ignore_opts,
            .visit_with_ignore = bx_rg_sched_walk_visit,
            .error = bx_rg_sched_walk_error,
            .inherited_parent_ignore_state = inherited_ignore,
        };
        size_t before = worker->stdout_len;
        int rc = bx_search_walk(work->u.dir.path, &walk_config, &walk_state);
        if (rc != 0)
            job_error_seen = true;
        job_match_seen = worker->stdout_len > before;
    } else {
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_BATCHES_SEARCHED, 1u);
        size_t before = worker->stdout_len;
        for (size_t i = 0; i < work->u.batch.count; ++i) {
            int status = bx_rg_sched_search_one(sched,
                                                worker,
                                                work->u.batch.items[i].path,
                                                work->u.batch.items[i].strip_dot_prefix,
                                                stolen);
            if (status == 2)
                job_error_seen = true;
            if (worker->stdout_len > before)
                job_match_seen = true;
            before = worker->stdout_len;
            if (bx_cancel_state_requested(&sched->cancel))
                break;
        }
    }

    bx_search_output_ctx_pop(previous_ctx);
    if (output_ctx.err)
        fclose(output_ctx.err);
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
    if (stderr_len > 0u)
        job_error_seen = true;

    if (worker->stdout_len == 0u && stderr_len == 0u && work->kind == BX_RG_SCHED_WORK_FILE_BATCH)
        bx_search_dev_counters_note_rg_sched(BX_SEARCH_RG_SCHED_EMPTY_BATCHES, 1u);

    if (worker->stdout_len == 0u && stderr_len == 0u) {
        free(stderr_buf);
        /*
         * Keep no-output work off the unordered publication path entirely.
         * Search results that emitted nothing merge directly.
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

        pthread_mutex_lock(&sched->lock);
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

static bool bx_rg_sched_build_frontier_for_root(struct bx_rg_sched_state *sched,
                                                struct bx_rg_sched_work_vec *vec,
                                                const char *root,
                                                bool strip_dot_prefix) {
    struct bx_walk_opts walk_opts = bx_search_make_walk_opts(sched->progname,
                                                             sched->personality,
                                                             sched->opts,
                                                             NULL);
    if (walk_opts.max_depth < 0 || walk_opts.max_depth > 1)
        walk_opts.max_depth = 1;
    struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(sched->opts);
    struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(sched->progname,
                                                                        sched->opts);
    struct bx_rg_sched_frontier_state frontier = {
        .sched = sched,
        .vec = vec,
        .root_path = root,
        .strip_dot_prefix = strip_dot_prefix,
    };
    struct bx_search_walk_config walk_config = {
        .walk_opts = &walk_opts,
        .filter_opts = &filter_opts,
        .ignore_opts = &ignore_opts,
        .visit_with_ignore = bx_rg_sched_frontier_visit,
        .error = bx_rg_sched_frontier_error,
    };
    int rc = bx_search_walk(root, &walk_config, &frontier);
    return rc == 0 && bx_rg_sched_flush_pending_batch(&frontier);
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

bool bx_rg_sched_supported(enum bx_search_personality personality,
                           const struct search_opts *opts,
                           int num_files,
                           bool rg_searches_stdin) {
    if (!opts || personality != BX_SEARCH_RG)
        return false;
    if (opts->files_only || opts->trace || opts->quiet || opts->stats || rg_searches_stdin)
        return false;
    if (!(opts->files_with_matches || opts->files_without_match))
        return false;
    if (bx_search_sort_requested(opts))
        return false;
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
    sched.aggregate = (struct bx_rg_publish_aggregate){
        .stats = &sched.stats,
        .exit_status = &sched.exit_status,
        .match_seen = &sched.match_seen,
        .error_seen = &sched.error_seen,
        .heading_output_started = &sched.heading_output_started,
    };

    bx_cancel_state_init(&sched.cancel);
    if (pthread_mutex_init(&sched.lock, NULL) != 0)
        return 2;
    if (pthread_cond_init(&sched.work_ready, NULL) != 0) {
        pthread_mutex_destroy(&sched.lock);
        return 2;
    }

    int num_files = argc - first_file;
    if (num_files == 0) {
        if (!bx_rg_sched_build_frontier_for_root(&sched, &frontier, ".", true)) {
            if (!sched.error_seen)
                bx_rg_sched_set_fatal(&sched, "rg: failed to build recursive frontier\n");
        }
    } else {
        for (int operand_i = 0; operand_i < num_files; ++operand_i) {
            int j = sorted_operands
                        ? sorted_operands[bx_search_sort_is_descending(opts)
                                              ? (sorted_operand_count - 1 - operand_i)
                                              : operand_i]
                              .index
                        : (first_file + operand_i);
            struct stat st;
            if (stat(argv[j], &st) != 0) {
                bx_rg_sched_report_path_error(&sched, argv[j], errno);
                sched.error_seen = true;
                sched.exit_status = 2;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (!bx_rg_sched_build_frontier_for_root(&sched, &frontier, argv[j], false)) {
                    if (!sched.error_seen)
                        bx_rg_sched_set_fatal(&sched, "rg: failed to build recursive frontier\n");
                }
                continue;
            }
            if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                continue;
            if (!bx_search_explicit_entry_selected(opts, argv[j]))
                continue;
            if (bx_search_path_exceeds_max_filesize(argv[j], opts))
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
            .user = &sched.aggregate,
            .record_seq = NULL,
            .emit_record = bx_rg_publish_emit_record_default,
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
