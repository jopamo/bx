#ifndef BX_LIB_WORK_POOL_H
#define BX_LIB_WORK_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

#include "cancel_state.h"

struct bx_work_pool_opts {
    size_t thread_count;
    size_t queue_capacity;
    void *user;
    const struct bx_cancel_state *cancel;
    void *(*worker_init)(void *user, size_t worker_index);
    void (*worker_fini)(void *user, void *worker_local, size_t worker_index);
    void (*process_job)(void *user, void *worker_local, void *job, size_t worker_index);
    void (*dispose_job)(void *user, void *job);
};

/*
 * Single-owner job lifecycle:
 *
 * The builder owns each mutable job before bx_work_pool_submit. A successful
 * submit transfers ownership to the pool; a worker owns the job while
 * process_job runs. A failed submit does not consume the job. dispose joins all
 * started workers before reclaiming any queued leftovers with dispose_job.
 */
struct bx_work_pool_thread_arg {
    struct bx_work_pool *pool;
    size_t worker_index;
};

struct bx_work_pool {
    struct bx_work_pool_opts opts;
    pthread_mutex_t lock;
    pthread_cond_t can_push;
    pthread_cond_t can_pop;
    pthread_t *threads;
    struct bx_work_pool_thread_arg *thread_args;
    void **items;
    size_t thread_count;
    size_t started_threads;
    size_t queue_capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool closed;
    bool failed;
    bool joined;
};

bool bx_work_pool_init(struct bx_work_pool *pool, const struct bx_work_pool_opts *opts);
bool bx_work_pool_submit(struct bx_work_pool *pool, void *job);
void bx_work_pool_close(struct bx_work_pool *pool);
void bx_work_pool_wake(struct bx_work_pool *pool);
bool bx_work_pool_join(struct bx_work_pool *pool);
void bx_work_pool_dispose(struct bx_work_pool *pool);

#endif
