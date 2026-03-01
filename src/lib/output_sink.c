#include <stdlib.h>
#include <string.h>

#include "output_sink.h"

struct bx_output_sink_node {
    void *record;
    uint64_t seq;
    struct bx_output_sink_node *next;
};

static void bx_output_sink_insert_node(struct bx_output_sink *sink,
                                       struct bx_output_sink_node *node) {
    if (!sink->head || !sink->opts.ordered || node->seq < sink->head->seq) {
        node->next = sink->head;
        sink->head = node;
        return;
    }

    struct bx_output_sink_node *it = sink->head;
    while (it->next && it->next->seq <= node->seq)
        it = it->next;
    node->next = it->next;
    it->next = node;
}

static void bx_output_sink_consume_skipped_locked(struct bx_output_sink *sink) {
    while (sink->skipped_len > 0u && sink->skipped_seqs[0] == sink->next_seq) {
        sink->next_seq++;
        sink->skipped_len--;
        if (sink->skipped_len > 0u) {
            memmove(sink->skipped_seqs, sink->skipped_seqs + 1u,
                    sink->skipped_len * sizeof(*sink->skipped_seqs));
        }
    }
}

static struct bx_output_sink_node *bx_output_sink_take_ready_node(struct bx_output_sink *sink) {
    struct bx_output_sink_node *node = sink->head;

    if (!node)
        return NULL;
    if (sink->opts.ordered && node->seq != sink->next_seq) {
        if (!sink->closed)
            return NULL;
    }

    sink->head = node->next;
    node->next = NULL;
    sink->pending--;
    if (sink->opts.ordered && node->seq == sink->next_seq) {
        sink->next_seq++;
        bx_output_sink_consume_skipped_locked(sink);
    }
    pthread_cond_signal(&sink->can_submit);
    return node;
}

static void *bx_output_sink_thread_main(void *arg) {
    struct bx_output_sink *sink = arg;

    for (;;) {
        struct bx_output_sink_node *node = NULL;

        pthread_mutex_lock(&sink->lock);
        while (!(node = bx_output_sink_take_ready_node(sink)) &&
               !(sink->closed && sink->pending == 0u)) {
            pthread_cond_wait(&sink->can_emit, &sink->lock);
        }
        if (!node && sink->closed && sink->pending == 0u) {
            pthread_mutex_unlock(&sink->lock);
            break;
        }
        pthread_mutex_unlock(&sink->lock);

        sink->opts.emit_record(sink->opts.user, node->record);
        if (sink->opts.dispose_record)
            sink->opts.dispose_record(sink->opts.user, node->record);
        free(node);
    }

    return NULL;
}

bool bx_output_sink_init(struct bx_output_sink *sink, const struct bx_output_sink_opts *opts) {
    if (!sink || !opts || !opts->record_seq || !opts->emit_record || opts->max_pending == 0u)
        return false;

    memset(sink, 0, sizeof(*sink));
    sink->opts = *opts;
    sink->next_seq = opts->first_seq;

    if (pthread_mutex_init(&sink->lock, NULL) != 0)
        return false;
    if (pthread_cond_init(&sink->can_submit, NULL) != 0) {
        pthread_mutex_destroy(&sink->lock);
        return false;
    }
    if (pthread_cond_init(&sink->can_emit, NULL) != 0) {
        pthread_cond_destroy(&sink->can_submit);
        pthread_mutex_destroy(&sink->lock);
        return false;
    }
    if (pthread_create(&sink->thread, NULL, bx_output_sink_thread_main, sink) != 0) {
        pthread_cond_destroy(&sink->can_emit);
        pthread_cond_destroy(&sink->can_submit);
        pthread_mutex_destroy(&sink->lock);
        return false;
    }
    sink->started = true;
    return true;
}

bool bx_output_sink_submit(struct bx_output_sink *sink, void *record) {
    struct bx_output_sink_node *node;

    if (!sink || !record)
        return false;

    node = calloc(1u, sizeof(*node));
    if (!node)
        return false;
    node->record = record;
    node->seq = sink->opts.record_seq(record, sink->opts.user);

    pthread_mutex_lock(&sink->lock);
    while (!sink->closed && sink->pending >= sink->opts.max_pending)
        pthread_cond_wait(&sink->can_submit, &sink->lock);
    if (sink->closed) {
        pthread_mutex_unlock(&sink->lock);
        free(node);
        return false;
    }

    bx_output_sink_insert_node(sink, node);
    sink->pending++;
    pthread_cond_signal(&sink->can_emit);
    pthread_mutex_unlock(&sink->lock);
    return true;
}

bool bx_output_sink_skip_seq(struct bx_output_sink *sink, uint64_t seq) {
    size_t pos;
    uint64_t *tmp;
    size_t new_cap;

    if (!sink)
        return false;
    if (!sink->opts.ordered)
        return true;

    pthread_mutex_lock(&sink->lock);
    if (seq < sink->next_seq) {
        pthread_mutex_unlock(&sink->lock);
        return true;
    }

    for (pos = 0u; pos < sink->skipped_len && sink->skipped_seqs[pos] < seq; pos++)
        ;
    if (pos < sink->skipped_len && sink->skipped_seqs[pos] == seq) {
        pthread_mutex_unlock(&sink->lock);
        return true;
    }

    if (sink->skipped_len == sink->skipped_cap) {
        new_cap = sink->skipped_cap == 0u ? 32u : sink->skipped_cap * 2u;
        tmp = realloc(sink->skipped_seqs, new_cap * sizeof(*sink->skipped_seqs));
        if (!tmp) {
            pthread_mutex_unlock(&sink->lock);
            return false;
        }
        sink->skipped_seqs = tmp;
        sink->skipped_cap = new_cap;
    }

    if (pos < sink->skipped_len) {
        memmove(sink->skipped_seqs + pos + 1u, sink->skipped_seqs + pos,
                (sink->skipped_len - pos) * sizeof(*sink->skipped_seqs));
    }
    sink->skipped_seqs[pos] = seq;
    sink->skipped_len++;
    bx_output_sink_consume_skipped_locked(sink);
    pthread_cond_signal(&sink->can_emit);
    pthread_mutex_unlock(&sink->lock);
    return true;
}

void bx_output_sink_close(struct bx_output_sink *sink) {
    if (!sink)
        return;

    pthread_mutex_lock(&sink->lock);
    sink->closed = true;
    pthread_cond_broadcast(&sink->can_submit);
    pthread_cond_broadcast(&sink->can_emit);
    pthread_mutex_unlock(&sink->lock);
}

void bx_output_sink_wake(struct bx_output_sink *sink) {
    if (!sink)
        return;

    pthread_mutex_lock(&sink->lock);
    pthread_cond_broadcast(&sink->can_submit);
    pthread_cond_broadcast(&sink->can_emit);
    pthread_mutex_unlock(&sink->lock);
}

bool bx_output_sink_join(struct bx_output_sink *sink) {
    if (!sink || !sink->started)
        return false;
    return pthread_join(sink->thread, NULL) == 0;
}

void bx_output_sink_dispose(struct bx_output_sink *sink) {
    struct bx_output_sink_node *node;

    if (!sink)
        return;

    node = sink->head;
    while (node) {
        struct bx_output_sink_node *next = node->next;
        if (sink->opts.dispose_record)
            sink->opts.dispose_record(sink->opts.user, node->record);
        free(node);
        node = next;
    }
    free(sink->skipped_seqs);

    pthread_cond_destroy(&sink->can_emit);
    pthread_cond_destroy(&sink->can_submit);
    pthread_mutex_destroy(&sink->lock);
    memset(sink, 0, sizeof(*sink));
}
