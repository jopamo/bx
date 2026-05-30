#include <stdlib.h>
#include <string.h>

#include "work_pool.h"

static bool bx_work_pool_cancelled(struct bx_work_pool *pool) {
    if (!pool || !pool->opts.cancel || !bx_cancel_state_requested(pool->opts.cancel))
        return false;
    (void)bx_cancel_state_mark_observed(pool->opts.cancel);
    return true;
}

static void *bx_work_pool_worker_main(void *arg) {
    struct bx_work_pool_thread_arg *thread_arg = arg;
    struct bx_work_pool *pool = thread_arg->pool;
    size_t worker_index = thread_arg->worker_index;
    void *worker_local = NULL;

    if (pool->opts.worker_init)
        worker_local = pool->opts.worker_init(pool->opts.user, worker_index);
    if (pool->opts.worker_init && !worker_local) {
        pthread_mutex_lock(&pool->lock);
        pool->failed = true;
        bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                         BX_WORKQUEUE_PROFILE_WAKE_PRODUCER,
                                         true);
        bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                         BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
                                         true);
        pthread_cond_broadcast(&pool->can_push);
        pthread_cond_broadcast(&pool->can_pop);
        pthread_mutex_unlock(&pool->lock);
        return NULL;
    }

    for (;;) {
        void *job = NULL;

        pthread_mutex_lock(&pool->lock);
        while (pool->count == 0u && !pool->closed) {
            uint_fast64_t wait_start =
                bx_workqueue_profile_wait_begin(pool->opts.profile);
            pthread_cond_wait(&pool->can_pop, &pool->lock);
            bx_workqueue_profile_wait_end(pool->opts.profile,
                                          BX_WORKQUEUE_PROFILE_CONSUMER_WAIT,
                                          wait_start);
        }
        if (pool->count == 0u && pool->closed) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        job = pool->items[pool->head];
        pool->items[pool->head] = NULL;
        bool was_full = pool->count == pool->queue_capacity;
        pool->head = (pool->head + 1u) % pool->queue_capacity;
        pool->count--;
        if (was_full) {
            bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                             BX_WORKQUEUE_PROFILE_WAKE_PRODUCER,
                                             false);
            pthread_cond_signal(&pool->can_push);
        }
        pthread_mutex_unlock(&pool->lock);

        pool->opts.process_job(pool->opts.user, worker_local, job, worker_index);
        bx_workqueue_profile_note_complete(pool->opts.profile);
    }

    if (pool->opts.worker_fini)
        pool->opts.worker_fini(pool->opts.user, worker_local, worker_index);
    return NULL;
}

bool bx_work_pool_init(struct bx_work_pool *pool, const struct bx_work_pool_opts *opts) {
    if (!pool || !opts || !opts->process_job || opts->thread_count == 0u || opts->queue_capacity == 0u)
        return false;
    if (opts->queue_limit_kind != BX_BACKPRESSURE_LIMIT_NONE &&
        !bx_backpressure_limit_kind_valid(opts->queue_limit_kind))
        return false;

    memset(pool, 0, sizeof(*pool));
    pool->opts = *opts;
    pool->thread_count = opts->thread_count;
    pool->queue_capacity = opts->queue_capacity;

    if (pthread_mutex_init(&pool->lock, NULL) != 0)
        return false;
    if (pthread_cond_init(&pool->can_push, NULL) != 0) {
        pthread_mutex_destroy(&pool->lock);
        return false;
    }
    if (pthread_cond_init(&pool->can_pop, NULL) != 0) {
        pthread_cond_destroy(&pool->can_push);
        pthread_mutex_destroy(&pool->lock);
        return false;
    }

    pool->threads = calloc(pool->thread_count, sizeof(*pool->threads));
    pool->thread_args = calloc(pool->thread_count, sizeof(*pool->thread_args));
    pool->items = calloc(pool->queue_capacity, sizeof(*pool->items));
    if (!pool->threads || !pool->thread_args || !pool->items) {
        bx_work_pool_dispose(pool);
        return false;
    }

    for (size_t i = 0; i < pool->thread_count; i++) {
        pool->thread_args[i] = (struct bx_work_pool_thread_arg){
            .pool = pool,
            .worker_index = i,
        };
        if (pthread_create(&pool->threads[i], NULL, bx_work_pool_worker_main,
                           &pool->thread_args[i]) != 0) {
            pthread_mutex_lock(&pool->lock);
            pool->closed = true;
            pool->failed = true;
            bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                             BX_WORKQUEUE_PROFILE_WAKE_PRODUCER,
                                             true);
            bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                             BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
                                             true);
            pthread_cond_broadcast(&pool->can_push);
            pthread_cond_broadcast(&pool->can_pop);
            pthread_mutex_unlock(&pool->lock);
            (void)bx_work_pool_join(pool);
            bx_work_pool_dispose(pool);
            return false;
        }
        pool->started_threads++;
    }

    return true;
}

bool bx_work_pool_submit(struct bx_work_pool *pool, void *job) {
    if (!pool)
        return false;

    pthread_mutex_lock(&pool->lock);
    while (!pool->closed && !pool->failed && pool->count == pool->queue_capacity) {
        if (bx_work_pool_cancelled(pool))
            break;
        uint_fast64_t wait_start =
            bx_workqueue_profile_wait_begin(pool->opts.profile);
        pthread_cond_wait(&pool->can_push, &pool->lock);
        bx_workqueue_profile_wait_end(pool->opts.profile,
                                      BX_WORKQUEUE_PROFILE_PRODUCER_WAIT,
                                      wait_start);
    }

    if (pool->closed || pool->failed || bx_work_pool_cancelled(pool)) {
        pthread_mutex_unlock(&pool->lock);
        return false;
    }

    pool->items[pool->tail] = job;
    pool->tail = (pool->tail + 1u) % pool->queue_capacity;
    pool->count++;
    bx_workqueue_profile_note_submit(pool->opts.profile);
    bx_workqueue_profile_note_depth(pool->opts.profile, pool->count);
    if (pool->count <= pool->thread_count) {
        bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                         BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
                                         false);
        pthread_cond_signal(&pool->can_pop);
    }
    pthread_mutex_unlock(&pool->lock);
    return true;
}

void bx_work_pool_close(struct bx_work_pool *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->closed = true;
    if (pool->opts.cancel && bx_cancel_state_requested(pool->opts.cancel)) {
        (void)bx_cancel_state_mark_observed(pool->opts.cancel);
        (void)bx_cancel_state_mark_draining(pool->opts.cancel);
    }
    bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                     BX_WORKQUEUE_PROFILE_WAKE_PRODUCER,
                                     true);
    bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                     BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
                                     true);
    pthread_cond_broadcast(&pool->can_push);
    pthread_cond_broadcast(&pool->can_pop);
    pthread_mutex_unlock(&pool->lock);
}

void bx_work_pool_wake(struct bx_work_pool *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                     BX_WORKQUEUE_PROFILE_WAKE_PRODUCER,
                                     true);
    bx_workqueue_profile_note_wakeup(pool->opts.profile,
                                     BX_WORKQUEUE_PROFILE_WAKE_CONSUMER,
                                     true);
    pthread_cond_broadcast(&pool->can_push);
    pthread_cond_broadcast(&pool->can_pop);
    pthread_mutex_unlock(&pool->lock);
}

bool bx_work_pool_join(struct bx_work_pool *pool) {
    bool ok = true;

    if (!pool)
        return false;
    if (pool->joined)
        return !pool->failed;

    for (size_t i = 0; i < pool->started_threads; i++) {
        if (pthread_join(pool->threads[i], NULL) != 0)
            ok = false;
    }

    pool->joined = true;
    if (pool->opts.cancel) {
        if (bx_cancel_state_requested(pool->opts.cancel)) {
            (void)bx_cancel_state_mark_observed(pool->opts.cancel);
            (void)bx_cancel_state_mark_draining(pool->opts.cancel);
            (void)bx_cancel_state_mark_joined(pool->opts.cancel);
        } else if (bx_cancel_state_draining(pool->opts.cancel)) {
            (void)bx_cancel_state_mark_joined(pool->opts.cancel);
        }
    }
    return ok && !pool->failed;
}

void bx_work_pool_dispose(struct bx_work_pool *pool) {
    if (!pool)
        return;

    /*
     * bx work pools are one-shot command resources. Reclaim queued jobs and
     * worker-owned state only after all workers have stopped; do not add
     * epoch/retire-list machinery for this lifecycle.
     */
    if (pool->started_threads > 0u && !pool->joined) {
        bx_work_pool_close(pool);
        (void)bx_work_pool_join(pool);
    }

    if (pool->items && pool->opts.dispose_job) {
        for (size_t i = 0; i < pool->queue_capacity; i++) {
            if (pool->items[i])
                pool->opts.dispose_job(pool->opts.user, pool->items[i]);
        }
    }

    free(pool->items);
    free(pool->thread_args);
    free(pool->threads);
    pthread_cond_destroy(&pool->can_pop);
    pthread_cond_destroy(&pool->can_push);
    pthread_mutex_destroy(&pool->lock);
    memset(pool, 0, sizeof(*pool));
}
