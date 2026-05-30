#ifndef BX_LIB_CANCEL_STATE_H
#define BX_LIB_CANCEL_STATE_H

#include <stdbool.h>
#include <stdatomic.h>

enum bx_cancel_state_phase {
    BX_CANCEL_STATE_REQUESTED = 1u << 0,
    BX_CANCEL_STATE_OBSERVED = 1u << 1,
    BX_CANCEL_STATE_DRAINING = 1u << 2,
    BX_CANCEL_STATE_KILLED = 1u << 3,
    BX_CANCEL_STATE_JOINED = 1u << 4,
    BX_CANCEL_STATE_PUBLISHED = 1u << 5,
    BX_CANCEL_STATE_ALL_PHASES = BX_CANCEL_STATE_REQUESTED |
                                 BX_CANCEL_STATE_OBSERVED |
                                 BX_CANCEL_STATE_DRAINING |
                                 BX_CANCEL_STATE_KILLED |
                                 BX_CANCEL_STATE_JOINED |
                                 BX_CANCEL_STATE_PUBLISHED,
};

struct bx_cancel_state {
    atomic_uint phases;
};

void bx_cancel_state_init(struct bx_cancel_state *state);
unsigned int bx_cancel_state_snapshot(const struct bx_cancel_state *state);
bool bx_cancel_state_has_phase(const struct bx_cancel_state *state,
                               enum bx_cancel_state_phase phase);
bool bx_cancel_state_mark_phase(struct bx_cancel_state *state,
                                enum bx_cancel_state_phase phase);
bool bx_cancel_state_requested(const struct bx_cancel_state *state);
bool bx_cancel_state_request(struct bx_cancel_state *state);
bool bx_cancel_state_mark_requested(struct bx_cancel_state *state);
bool bx_cancel_state_observed(const struct bx_cancel_state *state);
bool bx_cancel_state_mark_observed(struct bx_cancel_state *state);
bool bx_cancel_state_draining(const struct bx_cancel_state *state);
bool bx_cancel_state_mark_draining(struct bx_cancel_state *state);
bool bx_cancel_state_killed(const struct bx_cancel_state *state);
bool bx_cancel_state_mark_killed(struct bx_cancel_state *state);
bool bx_cancel_state_joined(const struct bx_cancel_state *state);
bool bx_cancel_state_mark_joined(struct bx_cancel_state *state);
bool bx_cancel_state_published(const struct bx_cancel_state *state);
bool bx_cancel_state_mark_published(struct bx_cancel_state *state);

#endif
