#ifndef BX_FETCH_RATE_LIMITER_H
#define BX_FETCH_RATE_LIMITER_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, net */

/*
 * Layering contract:
 * - Shared runtime token-bucket logic used by net dispatch.
 *
 * Ownership and lifetime:
 * - APIs operate on caller-owned BxFetchTokenBucket structs.
 * - No heap ownership is transferred by this interface.
 *
 * Preconditions:
 * - Callers should pass CLOCK_MONOTONIC timestamps to preserve refill invariants.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    int64_t rate_bytes_per_sec;
    double tokens;
    struct timespec last_refill;
    bool initialized;
} BxFetchTokenBucket;

void bx_fetch_token_bucket_init(BxFetchTokenBucket* bucket, int64_t rate_bytes_per_sec, const struct timespec* now);
double bx_fetch_token_bucket_consume(BxFetchTokenBucket* bucket, size_t bytes, const struct timespec* now);

#endif  // BX_FETCH_RATE_LIMITER_H
