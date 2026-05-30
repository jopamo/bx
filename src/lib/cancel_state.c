#include "cancel_state.h"

static bool bx_cancel_state_phase_valid(enum bx_cancel_state_phase phase) {
    unsigned int bit = (unsigned int)phase;

    return bit != 0u &&
           (bit & ~((unsigned int)BX_CANCEL_STATE_ALL_PHASES)) == 0u &&
           (bit & (bit - 1u)) == 0u;
}

void bx_cancel_state_init(struct bx_cancel_state *state) {
    if (!state)
        return;
    atomic_init(&state->phases, 0u);
}

unsigned int bx_cancel_state_snapshot(const struct bx_cancel_state *state) {
    if (!state)
        return 0u;
    return atomic_load_explicit(&state->phases, memory_order_acquire);
}

bool bx_cancel_state_has_phase(const struct bx_cancel_state *state,
                               enum bx_cancel_state_phase phase) {
    unsigned int bit = (unsigned int)phase;

    if (!state || !bx_cancel_state_phase_valid(phase))
        return false;
    return (bx_cancel_state_snapshot(state) & bit) != 0u;
}

bool bx_cancel_state_mark_phase(struct bx_cancel_state *state,
                                enum bx_cancel_state_phase phase) {
    unsigned int bit = (unsigned int)phase;
    unsigned int previous;

    if (!state || !bx_cancel_state_phase_valid(phase))
        return false;

    previous = atomic_fetch_or_explicit(&state->phases, bit, memory_order_release);
    return (previous & bit) == 0u;
}

bool bx_cancel_state_requested(const struct bx_cancel_state *state) {
    return state &&
           (atomic_load_explicit(&state->phases, memory_order_relaxed) &
            (unsigned int)BX_CANCEL_STATE_REQUESTED) != 0u;
}

bool bx_cancel_state_request(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_requested(state);
}

bool bx_cancel_state_mark_requested(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_REQUESTED);
}

bool bx_cancel_state_observed(const struct bx_cancel_state *state) {
    return bx_cancel_state_has_phase(state, BX_CANCEL_STATE_OBSERVED);
}

bool bx_cancel_state_mark_observed(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_OBSERVED);
}

bool bx_cancel_state_draining(const struct bx_cancel_state *state) {
    return bx_cancel_state_has_phase(state, BX_CANCEL_STATE_DRAINING);
}

bool bx_cancel_state_mark_draining(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_DRAINING);
}

bool bx_cancel_state_killed(const struct bx_cancel_state *state) {
    return bx_cancel_state_has_phase(state, BX_CANCEL_STATE_KILLED);
}

bool bx_cancel_state_mark_killed(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_KILLED);
}

bool bx_cancel_state_joined(const struct bx_cancel_state *state) {
    return bx_cancel_state_has_phase(state, BX_CANCEL_STATE_JOINED);
}

bool bx_cancel_state_mark_joined(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_JOINED);
}

bool bx_cancel_state_published(const struct bx_cancel_state *state) {
    return bx_cancel_state_has_phase(state, BX_CANCEL_STATE_PUBLISHED);
}

bool bx_cancel_state_mark_published(struct bx_cancel_state *state) {
    return bx_cancel_state_mark_phase(state, BX_CANCEL_STATE_PUBLISHED);
}
