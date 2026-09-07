#define _GNU_SOURCE
#include "lib/fetch/rate_limiter.h"
#include "lib/time_parse.h"

void bx_fetch_token_bucket_init(BxFetchTokenBucket* bucket, int64_t rate_bytes_per_sec, const struct timespec* now) {
    if (!bucket)
        return;

    bucket->rate_bytes_per_sec = rate_bytes_per_sec;
    bucket->tokens = 0.0;
    bucket->initialized = false;
    if (rate_bytes_per_sec <= 0 || !now)
        return;

    bucket->tokens = (double)rate_bytes_per_sec;
    bucket->last_refill = *now;
    bucket->initialized = true;
}

double bx_fetch_token_bucket_consume(BxFetchTokenBucket* bucket, size_t bytes, const struct timespec* now) {
    if (!bucket || bucket->rate_bytes_per_sec <= 0 || bytes == 0 || !now)
        return 0.0;

    if (!bucket->initialized) {
        bx_fetch_token_bucket_init(bucket, bucket->rate_bytes_per_sec, now);
    }

    double elapsed;
    if (bx_time_timespec_elapsed_seconds_double(&bucket->last_refill, now, &elapsed) && elapsed > 0.0) {
        bucket->tokens += elapsed * (double)bucket->rate_bytes_per_sec;
        if (bucket->tokens > (double)bucket->rate_bytes_per_sec) {
            bucket->tokens = (double)bucket->rate_bytes_per_sec;
        }
        bucket->last_refill = *now;
    }

    bucket->tokens -= (double)bytes;
    if (bucket->tokens >= 0.0)
        return 0.0;

    return (-bucket->tokens) / (double)bucket->rate_bytes_per_sec;
}
