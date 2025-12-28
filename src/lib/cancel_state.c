#include "cancel_state.h"

void bx_cancel_state_init(struct bx_cancel_state *state) {
    if (!state)
        return;
    atomic_init(&state->requested, false);
}

bool bx_cancel_state_requested(const struct bx_cancel_state *state) {
    return state && atomic_load_explicit(&state->requested, memory_order_relaxed);
}

bool bx_cancel_state_request(struct bx_cancel_state *state) {
    bool expected = false;

    if (!state)
        return false;
    return atomic_compare_exchange_strong_explicit(&state->requested,
                                                   &expected,
                                                   true,
                                                   memory_order_relaxed,
                                                   memory_order_relaxed);
}
