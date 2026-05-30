#ifndef BX_LIB_OUTPUT_PUBLICATION_H
#define BX_LIB_OUTPUT_PUBLICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "output_sink.h"

enum bx_output_publication_mode {
    BX_OUTPUT_PUBLICATION_ORDERED = 0,
    BX_OUTPUT_PUBLICATION_UNORDERED_FAST,
};

struct bx_output_publication_opts {
    enum bx_output_publication_mode mode;
    size_t max_pending;
    size_t max_pending_bytes;
    uint64_t first_seq;
    bool order_irrelevant;
    const char *order_irrelevant_reason;
    void *user;
    uint64_t (*record_seq)(const void *record, void *user);
    size_t (*record_size)(const void *record, void *user);
    void (*emit_record)(void *user, void *record);
    void (*dispose_record)(void *user, void *record);
};

struct bx_output_publication {
    struct bx_output_publication_opts opts;
    pthread_mutex_t unordered_lock;
    bool unordered_lock_ready;
    struct bx_output_sink ordered_sink;
    bool ordered_sink_ready;
};

/*
 * Shared output publication helper:
 *
 * ORDERED is deterministic by sequence number and uses bx_output_sink for the
 * bounded publisher queue. UNORDERED_FAST is accepted only when the caller
 * explicitly sets order_irrelevant plus a non-empty order_irrelevant_reason;
 * it does not allocate a queue node or start a publisher thread, and
 * emits/disposes under one short mutex.
 *
 * Successful submit consumes record ownership. Failed submit leaves caller
 * ownership unchanged. The unordered batch helper consumes every non-NULL
 * record only when the helper is in UNORDERED_FAST mode.
 */
bool bx_output_publication_init(struct bx_output_publication *publication,
                                const struct bx_output_publication_opts *opts);
bool bx_output_publication_submit(struct bx_output_publication *publication,
                                  void *record);
bool bx_output_publication_submit_unordered_batch(
    struct bx_output_publication *publication,
    void **records,
    size_t count);
bool bx_output_publication_skip_seq(struct bx_output_publication *publication,
                                    uint64_t seq);
void bx_output_publication_lock_unordered_fast(struct bx_output_publication *publication);
void bx_output_publication_unlock_unordered_fast(struct bx_output_publication *publication);
void bx_output_publication_close(struct bx_output_publication *publication);
void bx_output_publication_wake(struct bx_output_publication *publication);
bool bx_output_publication_join(struct bx_output_publication *publication);
void bx_output_publication_dispose(struct bx_output_publication *publication);

#endif /* BX_LIB_OUTPUT_PUBLICATION_H */
