#ifndef BX_LIB_OUTPUT_SINK_H
#define BX_LIB_OUTPUT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "backpressure_limit.h"
#include "workqueue_profile.h"
#include "workqueue_contract.h"

struct bx_output_sink_opts {
    size_t max_pending;
    size_t max_pending_bytes;
    uint64_t first_seq;
    bool ordered;
    void *user;
    struct bx_workqueue_profile_sink *profile;
    uint64_t (*record_seq)(const void *record, void *user);
    size_t (*record_size)(const void *record, void *user);
    void (*emit_record)(void *user, void *record);
    void (*dispose_record)(void *user, void *record);
};

/*
 * Output chunk workqueue contract:
 *
 * bx_output_sink implements BX_WORKQUEUE_CONTRACT_OUTPUT_CHUNKS for ordered
 * publication. max_pending is an explicit record-count bound; max_pending_bytes
 * is an optional BX_BACKPRESSURE_LIMIT_PENDING_OUTPUT_BYTES budget. Successful
 * submit transfers record ownership, failed submit preserves producer
 * ownership, and close happens before join/reclaim.
 *
 * Single-owner record lifecycle:
 *
 * The producer owns a mutable record before bx_output_sink_submit. A successful
 * submit transfers ownership to the sink; the emitter owns it while
 * emit_record runs and then dispose_record releases it. A failed submit does
 * not consume the record. dispose joins the emitter before draining leftovers.
 */
struct bx_output_sink {
    struct bx_output_sink_opts opts;
    pthread_mutex_t lock;
    pthread_cond_t can_submit;
    pthread_cond_t can_emit;
    pthread_t thread;
    struct bx_output_sink_node *head;
    uint64_t *skipped_seqs;
    size_t skipped_len;
    size_t skipped_cap;
    size_t pending;
    size_t pending_bytes;
    uint64_t next_seq;
    bool closed;
    bool failed;
    bool started;
    bool joined;
};

bool bx_output_sink_init(struct bx_output_sink *sink, const struct bx_output_sink_opts *opts);
bool bx_output_sink_submit(struct bx_output_sink *sink, void *record);
bool bx_output_sink_skip_seq(struct bx_output_sink *sink, uint64_t seq);
void bx_output_sink_close(struct bx_output_sink *sink);
void bx_output_sink_wake(struct bx_output_sink *sink);
bool bx_output_sink_join(struct bx_output_sink *sink);
void bx_output_sink_dispose(struct bx_output_sink *sink);

#endif
