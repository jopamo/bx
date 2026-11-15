#ifndef BX_LIB_COPY_QUEUE_H
#define BX_LIB_COPY_QUEUE_H

#include <stdbool.h>
#include "copy_data.h"

#define BX_COPY_MAX_WORKERS 8u

struct bx_copy_queue;

struct bx_copy_job {
    int src_fd;
    int dest_fd;
    int result;
    int error;
    dev_t dest_dev;
    ino_t dest_ino;
    bool (*finish)(struct bx_copy_job *job, void *owner);
    void *owner;

    /* Published under the queue lock; only the coordinator links jobs. */
    bool done;
    struct bx_copy_job *next;
};

struct bx_copy_queue *bx_copy_queue_create(unsigned workers,
                                          const struct bx_copy_data_options *options);
/*
 * Submit transfers descriptor use to a worker until completion. finish runs
 * on the submitting thread, in submission order, and must close descriptors
 * and reclaim the job. It must not submit more work to this queue.
 * Failed submission leaves ownership with the caller.
 */
bool bx_copy_queue_submit(struct bx_copy_queue *queue, struct bx_copy_job *job);
bool bx_copy_queue_drain(struct bx_copy_queue *queue);
bool bx_copy_queue_has_destination(const struct bx_copy_queue *queue, dev_t dev, ino_t ino);
/* Stop submissions and finish owned work before reclaiming worker state. */
void bx_copy_queue_destroy(struct bx_copy_queue *queue);

#endif
