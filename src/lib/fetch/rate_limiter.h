#ifndef MIRA_RATE_LIMITER_H
#define MIRA_RATE_LIMITER_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: runtime, net */

/*
 * Layering contract:
 * - Shared runtime token-bucket logic used by net dispatch.
 *
 * Ownership and lifetime:
 * - APIs operate on caller-owned MiraTokenBucket structs.
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
} MiraTokenBucket;

void mira_token_bucket_init(MiraTokenBucket* bucket, int64_t rate_bytes_per_sec, const struct timespec* now);
double mira_token_bucket_consume(MiraTokenBucket* bucket, size_t bytes, const struct timespec* now);

#endif  // MIRA_RATE_LIMITER_H
