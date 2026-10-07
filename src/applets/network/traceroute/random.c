/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include "traceroute.h"

unsigned int bx_traceroute_random_seq(struct bx_traceroute_ctx* ctx) {
    uint32_t value = ctx->random_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    ctx->random_state = value;
    return value;
}
