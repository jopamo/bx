#ifndef BX_LIB_CANCEL_STATE_H
#define BX_LIB_CANCEL_STATE_H

#include <stdbool.h>
#include <stdatomic.h>

struct bx_cancel_state {
    atomic_bool requested;
};

void bx_cancel_state_init(struct bx_cancel_state *state);
bool bx_cancel_state_requested(const struct bx_cancel_state *state);
bool bx_cancel_state_request(struct bx_cancel_state *state);

#endif
