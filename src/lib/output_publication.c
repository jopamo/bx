#include <string.h>

#include "output_publication.h"

static enum bx_output_publication_mode bx_output_publication_mode(
    const struct bx_output_publication *publication) {
    return publication ? publication->opts.mode : BX_OUTPUT_PUBLICATION_ORDERED;
}

static void bx_output_publication_emit_and_dispose_unlocked(
    struct bx_output_publication *publication,
    void *record) {
    if (!publication || !record)
        return;

    publication->opts.emit_record(publication->opts.user, record);
    if (publication->opts.dispose_record)
        publication->opts.dispose_record(publication->opts.user, record);
}

bool bx_output_publication_init(struct bx_output_publication *publication,
                                const struct bx_output_publication_opts *opts) {
    if (!publication || !opts || !opts->emit_record)
        return false;

    memset(publication, 0, sizeof(*publication));
    publication->opts = *opts;

    if (opts->mode == BX_OUTPUT_PUBLICATION_ORDERED) {
        if (!opts->record_seq || opts->max_pending == 0u)
            return false;
        struct bx_output_sink_opts sink_opts = {
            .max_pending = opts->max_pending,
            .max_pending_bytes = opts->max_pending_bytes,
            .first_seq = opts->first_seq,
            .ordered = true,
            .user = opts->user,
            .record_seq = opts->record_seq,
            .record_size = opts->record_size,
            .emit_record = opts->emit_record,
            .dispose_record = opts->dispose_record,
        };
        if (!bx_output_sink_init(&publication->ordered_sink, &sink_opts))
            return false;
        publication->ordered_sink_ready = true;
        return true;
    }

    if (opts->mode != BX_OUTPUT_PUBLICATION_UNORDERED_FAST)
        return false;
    if (!opts->order_irrelevant ||
        !opts->order_irrelevant_reason ||
        opts->order_irrelevant_reason[0] == '\0') {
        return false;
    }
    if (pthread_mutex_init(&publication->unordered_lock, NULL) != 0)
        return false;
    publication->unordered_lock_ready = true;
    return true;
}

bool bx_output_publication_submit(struct bx_output_publication *publication,
                                  void *record) {
    if (!publication || !record)
        return false;

    if (bx_output_publication_mode(publication) == BX_OUTPUT_PUBLICATION_ORDERED)
        return bx_output_sink_submit(&publication->ordered_sink, record);

    pthread_mutex_lock(&publication->unordered_lock);
    bx_output_publication_emit_and_dispose_unlocked(publication, record);
    pthread_mutex_unlock(&publication->unordered_lock);
    return true;
}

bool bx_output_publication_submit_unordered_batch(
    struct bx_output_publication *publication,
    void **records,
    size_t count) {
    if (!publication || (!records && count > 0u))
        return false;
    if (bx_output_publication_mode(publication) != BX_OUTPUT_PUBLICATION_UNORDERED_FAST)
        return false;

    pthread_mutex_lock(&publication->unordered_lock);
    for (size_t i = 0; i < count; i++)
        bx_output_publication_emit_and_dispose_unlocked(publication, records[i]);
    pthread_mutex_unlock(&publication->unordered_lock);
    return true;
}

bool bx_output_publication_skip_seq(struct bx_output_publication *publication,
                                    uint64_t seq) {
    if (!publication)
        return false;
    if (bx_output_publication_mode(publication) != BX_OUTPUT_PUBLICATION_ORDERED)
        return true;
    return bx_output_sink_skip_seq(&publication->ordered_sink, seq);
}

void bx_output_publication_lock_unordered_fast(struct bx_output_publication *publication) {
    if (!publication ||
        bx_output_publication_mode(publication) != BX_OUTPUT_PUBLICATION_UNORDERED_FAST ||
        !publication->unordered_lock_ready) {
        return;
    }
    pthread_mutex_lock(&publication->unordered_lock);
}

void bx_output_publication_unlock_unordered_fast(struct bx_output_publication *publication) {
    if (!publication ||
        bx_output_publication_mode(publication) != BX_OUTPUT_PUBLICATION_UNORDERED_FAST ||
        !publication->unordered_lock_ready) {
        return;
    }
    pthread_mutex_unlock(&publication->unordered_lock);
}

void bx_output_publication_close(struct bx_output_publication *publication) {
    if (!publication)
        return;
    if (bx_output_publication_mode(publication) == BX_OUTPUT_PUBLICATION_ORDERED)
        bx_output_sink_close(&publication->ordered_sink);
}

void bx_output_publication_wake(struct bx_output_publication *publication) {
    if (!publication)
        return;
    if (bx_output_publication_mode(publication) == BX_OUTPUT_PUBLICATION_ORDERED)
        bx_output_sink_wake(&publication->ordered_sink);
}

bool bx_output_publication_join(struct bx_output_publication *publication) {
    if (!publication)
        return false;
    if (bx_output_publication_mode(publication) != BX_OUTPUT_PUBLICATION_ORDERED)
        return true;
    return bx_output_sink_join(&publication->ordered_sink);
}

void bx_output_publication_dispose(struct bx_output_publication *publication) {
    if (!publication)
        return;
    if (publication->ordered_sink_ready)
        bx_output_sink_dispose(&publication->ordered_sink);
    if (publication->unordered_lock_ready)
        pthread_mutex_destroy(&publication->unordered_lock);
    memset(publication, 0, sizeof(*publication));
}
