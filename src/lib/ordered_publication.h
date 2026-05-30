#ifndef BX_LIB_ORDERED_PUBLICATION_H
#define BX_LIB_ORDERED_PUBLICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "cancel_state.h"

struct bx_ordered_publication_packet {
    uint64_t seq;
    struct bx_ordered_publication_packet *next;
};

typedef bool (*bx_ordered_publication_publish_fn)(
    void *user,
    struct bx_ordered_publication_packet *packet);
typedef void (*bx_ordered_publication_dispose_fn)(
    void *user,
    struct bx_ordered_publication_packet *packet);

struct bx_ordered_publication_opts {
    struct bx_cancel_state *cancel;
    size_t max_inflight;
    void *user;
    bx_ordered_publication_publish_fn publish;
    bx_ordered_publication_dispose_fn dispose;
};

/*
 * Deterministic ordered-publication window for parallel applets:
 *
 * The caller reserves a sequence/in-flight slot before handing a packet to a
 * worker queue. Workers publish completed packets back to this helper. The
 * caller drains contiguous sequence numbers through the applet-owned publish
 * callback, preserving user-visible order while bounding queued, active, and
 * completed-not-yet-published packets with one max_inflight limit.
 *
 * Applet code owns packet payload policy. This helper owns sequence assignment,
 * in-flight accounting, ordered pending storage, wakeups, and publication
 * failure cancellation phases.
 */
struct bx_ordered_publication_state {
    struct bx_cancel_state *cancel;
    void *user;
    bx_ordered_publication_publish_fn publish;
    bx_ordered_publication_dispose_fn dispose;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    struct bx_ordered_publication_packet *pending_head;
    size_t max_inflight;
    size_t inflight;
    uint64_t next_submit_seq;
    uint64_t next_publish_seq;
    bool lock_initialized;
    bool cond_initialized;
};

bool bx_ordered_publication_init(
    struct bx_ordered_publication_state *state,
    const struct bx_ordered_publication_opts *opts);
void bx_ordered_publication_cleanup(struct bx_ordered_publication_state *state);

bool bx_ordered_publication_reserve_slot(
    struct bx_ordered_publication_state *state,
    uint64_t *seq_out);
void bx_ordered_publication_release_slot(
    struct bx_ordered_publication_state *state);

void bx_ordered_publication_publish_packet(
    struct bx_ordered_publication_state *state,
    struct bx_ordered_publication_packet *packet);
bool bx_ordered_publication_drain(struct bx_ordered_publication_state *state);

#endif /* BX_LIB_ORDERED_PUBLICATION_H */
