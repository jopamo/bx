#include <string.h>

#include "applets/archive/archive_ordered.h"

static void bx_archive_ordered_insert_pending(struct bx_archive_ordered_state* state,
                                              struct bx_archive_ordered_packet* packet) {
    struct bx_archive_ordered_packet** link = &state->pending_head;

    while (*link != NULL && (*link)->seq < packet->seq) {
        link = &(*link)->next;
    }
    packet->next = *link;
    *link = packet;
}

static bool bx_archive_ordered_flush_ready(struct bx_archive_ordered_state* state) {
    for (;;) {
        struct bx_archive_ordered_packet* packet = NULL;
        bool ok = true;

        pthread_mutex_lock(&state->lock);
        if (state->pending_head != NULL
            && state->pending_head->seq == state->next_publish_seq) {
            packet = state->pending_head;
            state->pending_head = packet->next;
            packet->next = NULL;
            state->next_publish_seq++;
        }
        pthread_mutex_unlock(&state->lock);

        if (packet == NULL) {
            return true;
        }

        if (!state->publish(state->user, packet)) {
            ok = false;
            if (state->cancel != NULL) {
                bx_cancel_state_request(state->cancel);
            }
        }

        pthread_mutex_lock(&state->lock);
        if (state->inflight > 0u) {
            state->inflight--;
        }
        pthread_cond_broadcast(&state->cond);
        pthread_mutex_unlock(&state->lock);

        if (state->dispose != NULL) {
            state->dispose(state->user, packet);
        }

        if (!ok) {
            return false;
        }
    }
}

static void bx_archive_ordered_wait_for_ready(struct bx_archive_ordered_state* state) {
    pthread_mutex_lock(&state->lock);
    while ((state->pending_head == NULL
            || state->pending_head->seq != state->next_publish_seq)
           && state->inflight > 0u) {
        pthread_cond_wait(&state->cond, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

bool bx_archive_ordered_init(struct bx_archive_ordered_state* state,
                             const struct bx_archive_ordered_opts* opts) {
    if (state == NULL || opts == NULL || opts->max_inflight == 0u || opts->publish == NULL) {
        return false;
    }

    memset(state, 0, sizeof(*state));
    state->cancel = opts->cancel;
    state->max_inflight = opts->max_inflight;
    state->user = opts->user;
    state->publish = opts->publish;
    state->dispose = opts->dispose;

    if (pthread_mutex_init(&state->lock, NULL) != 0) {
        return false;
    }
    state->lock_initialized = true;
    if (pthread_cond_init(&state->cond, NULL) != 0) {
        pthread_mutex_destroy(&state->lock);
        state->lock_initialized = false;
        return false;
    }
    state->cond_initialized = true;

    return true;
}

void bx_archive_ordered_cleanup(struct bx_archive_ordered_state* state) {
    struct bx_archive_ordered_packet* packet;

    if (state == NULL) {
        return;
    }

    while ((packet = state->pending_head) != NULL) {
        state->pending_head = packet->next;
        if (state->dispose != NULL) {
            state->dispose(state->user, packet);
        }
    }
    if (state->cond_initialized) {
        pthread_cond_destroy(&state->cond);
    }
    if (state->lock_initialized) {
        pthread_mutex_destroy(&state->lock);
    }
    memset(state, 0, sizeof(*state));
}

bool bx_archive_ordered_reserve_slot(struct bx_archive_ordered_state* state,
                                     uint64_t* seq_out) {
    if (state == NULL || seq_out == NULL) {
        return false;
    }

    while (true) {
        if (!bx_archive_ordered_flush_ready(state)) {
            return false;
        }

        pthread_mutex_lock(&state->lock);
        if (state->inflight < state->max_inflight) {
            *seq_out = state->next_submit_seq++;
            state->inflight++;
            pthread_mutex_unlock(&state->lock);
            return true;
        }
        pthread_mutex_unlock(&state->lock);

        bx_archive_ordered_wait_for_ready(state);
    }
}

void bx_archive_ordered_release_slot(struct bx_archive_ordered_state* state) {
    if (state == NULL) {
        return;
    }

    pthread_mutex_lock(&state->lock);
    if (state->inflight > 0u) {
        state->inflight--;
    }
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->lock);
}

void bx_archive_ordered_publish_packet(struct bx_archive_ordered_state* state,
                                       struct bx_archive_ordered_packet* packet) {
    if (state == NULL || packet == NULL) {
        return;
    }

    pthread_mutex_lock(&state->lock);
    bx_archive_ordered_insert_pending(state, packet);
    pthread_cond_broadcast(&state->cond);
    pthread_mutex_unlock(&state->lock);
}

bool bx_archive_ordered_drain(struct bx_archive_ordered_state* state) {
    if (state == NULL) {
        return false;
    }

    while (true) {
        size_t inflight;

        if (!bx_archive_ordered_flush_ready(state)) {
            return false;
        }

        pthread_mutex_lock(&state->lock);
        inflight = state->inflight;
        pthread_mutex_unlock(&state->lock);
        if (inflight == 0u) {
            return true;
        }

        bx_archive_ordered_wait_for_ready(state);
    }
}
