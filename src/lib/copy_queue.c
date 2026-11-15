#include <errno.h>
#include <stdlib.h>

#include "copy_queue.h"
#include "work_pool.h"

struct bx_copy_queue {
    struct bx_work_pool pool;
    struct bx_copy_data_options options;
    struct bx_copy_buffer buffers[BX_COPY_MAX_WORKERS];
    pthread_mutex_t lock;
    pthread_cond_t completed;
    struct bx_copy_job *head;
    struct bx_copy_job *tail;
    size_t count;
    size_t capacity;
    bool failed;
};

static void bx_copy_queue_process(void *user, void *local, void *item, size_t index) {
    struct bx_copy_queue *queue = user;
    struct bx_copy_job *job = item;
    (void)local;

    int result = bx_copy_data_with_buffer(job->src_fd, job->dest_fd,
                                          &queue->options, &queue->buffers[index]);
    int error = errno;
    pthread_mutex_lock(&queue->lock);
    job->result = result;
    job->error = error;
    job->done = true;
    pthread_cond_signal(&queue->completed);
    pthread_mutex_unlock(&queue->lock);
}

static void bx_copy_queue_finish_first(struct bx_copy_queue *queue) {
    struct bx_copy_job *job = queue->head;

    pthread_mutex_lock(&queue->lock);
    while (!job->done)
        pthread_cond_wait(&queue->completed, &queue->lock);
    pthread_mutex_unlock(&queue->lock);

    queue->head = job->next;
    if (queue->head == NULL)
        queue->tail = NULL;
    queue->count--;
    if (!job->finish(job, job->owner))
        queue->failed = true;
}

struct bx_copy_queue *bx_copy_queue_create(unsigned workers,
                                          const struct bx_copy_data_options *options) {
    if (workers < 2 || workers > BX_COPY_MAX_WORKERS)
        return NULL;

    struct bx_copy_queue *queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
        return NULL;
    queue->options = *options;
    queue->capacity = workers * 2u;
    size_t files = bx_backpressure_limit_default(BX_BACKPRESSURE_LIMIT_PENDING_FILES);
    size_t fds = bx_backpressure_limit_default(BX_BACKPRESSURE_LIMIT_OPEN_FDS) / 2u;
    if (queue->capacity > files)
        queue->capacity = files;
    if (queue->capacity > fds)
        queue->capacity = fds;
    if (pthread_mutex_init(&queue->lock, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&queue->completed, NULL) != 0)
        goto fail_lock;
    struct bx_work_pool_opts opts = {
        .thread_count = workers,
        .queue_capacity = queue->capacity,
        .queue_limit_kind = BX_BACKPRESSURE_LIMIT_PENDING_FILES,
        .user = queue,
        .process_job = bx_copy_queue_process,
    };
    if (!bx_work_pool_init(&queue->pool, &opts))
        goto fail_cond;
    return queue;

fail_cond:
    pthread_cond_destroy(&queue->completed);
fail_lock:
    pthread_mutex_destroy(&queue->lock);
fail:
    free(queue);
    return NULL;
}

bool bx_copy_queue_submit(struct bx_copy_queue *queue, struct bx_copy_job *job) {
    /* Count running and completed jobs as well as jobs awaiting a worker. */
    if (queue->count == queue->capacity)
        bx_copy_queue_finish_first(queue);
    job->done = false;
    job->next = NULL;
    if (!bx_work_pool_submit(&queue->pool, job))
        return false;
    if (queue->tail != NULL)
        queue->tail->next = job;
    else
        queue->head = job;
    queue->tail = job;
    queue->count++;
    return true;
}

bool bx_copy_queue_drain(struct bx_copy_queue *queue) {
    if (queue == NULL)
        return true;
    while (queue->head != NULL)
        bx_copy_queue_finish_first(queue);
    return !queue->failed;
}

bool bx_copy_queue_has_destination(const struct bx_copy_queue *queue, dev_t dev, ino_t ino) {
    if (queue != NULL) {
        for (const struct bx_copy_job *job = queue->head; job != NULL; job = job->next) {
            if (job->dest_dev == dev && job->dest_ino == ino)
                return true;
        }
    }
    return false;
}

void bx_copy_queue_destroy(struct bx_copy_queue *queue) {
    if (queue == NULL)
        return;
    bx_work_pool_close(&queue->pool);
    (void)bx_copy_queue_drain(queue);
    bx_work_pool_dispose(&queue->pool);
    for (size_t i = 0; i < BX_COPY_MAX_WORKERS; i++)
        bx_copy_buffer_release(&queue->buffers[i]);
    pthread_cond_destroy(&queue->completed);
    pthread_mutex_destroy(&queue->lock);
    free(queue);
}
