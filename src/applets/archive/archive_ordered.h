#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_ORDERED_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_ORDERED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "lib/cancel_state.h"

struct bx_archive_ordered_packet {
    uint64_t seq;
    struct bx_archive_ordered_packet* next;
};

typedef bool (*bx_archive_ordered_publish_fn)(void* user, struct bx_archive_ordered_packet* packet);
typedef void (*bx_archive_ordered_dispose_fn)(void* user, struct bx_archive_ordered_packet* packet);

struct bx_archive_ordered_opts {
    struct bx_cancel_state* cancel;
    size_t max_inflight;
    void* user;
    bx_archive_ordered_publish_fn publish;
    bx_archive_ordered_dispose_fn dispose;
};

struct bx_archive_ordered_state {
    struct bx_cancel_state* cancel;
    void* user;
    bx_archive_ordered_publish_fn publish;
    bx_archive_ordered_dispose_fn dispose;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct bx_archive_ordered_packet* pending_head;
    size_t max_inflight;
    size_t inflight;
    uint64_t next_submit_seq;
    uint64_t next_publish_seq;
    bool lock_initialized;
    bool cond_initialized;
};

bool bx_archive_ordered_init(struct bx_archive_ordered_state* state,
                             const struct bx_archive_ordered_opts* opts);
void bx_archive_ordered_cleanup(struct bx_archive_ordered_state* state);

bool bx_archive_ordered_reserve_slot(struct bx_archive_ordered_state* state,
                                     uint64_t* seq_out);
void bx_archive_ordered_release_slot(struct bx_archive_ordered_state* state);

void bx_archive_ordered_publish_packet(struct bx_archive_ordered_state* state,
                                       struct bx_archive_ordered_packet* packet);
bool bx_archive_ordered_drain(struct bx_archive_ordered_state* state);

#endif
