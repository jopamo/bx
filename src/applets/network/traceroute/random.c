/*
    Copyright (c)  2006, 2007		Dmitry Butskoy
                                        <dmitry@butskoy.name>
    License:  GPL v2 or any later

    See COPYING for the status of this software.
*/

#include <stdlib.h>
#include <unistd.h>
#include <sys/times.h>

#include "lib/random_bytes.h"
#include "traceroute.h"

static void __init_random_seq(void) __attribute__((constructor));
static void __init_random_seq(void) {
    // Prefer shared nonblocking system entropy.
    unsigned int seed = 0;

    // Do not let startup wait for entropy; this seed has a local fallback.
    if (!bx_random_bytes_nonblocking(&seed, sizeof(seed))) {
        // Fall back to time and PID when system entropy is unavailable.
        seed = times(NULL) + getpid();
    }

    srand(seed);
}

unsigned int random_seq(void) {
    // Using random() instead of rand() for better randomness
    return (random() << 16) ^ (random() << 8) ^ random() ^ (random() >> 8);
}
