#ifndef BX_FETCH_SCHEDULER_H
#define BX_FETCH_SCHEDULER_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core */

/*
 * Layering contract:
 * - BxFetchScheduler controls dispatch order/retry timing and is used only by core.
 * - Network and policy layers report completion decisions via callbacks; they do
 *   not mutate scheduler internals.
 *
 * Ownership and lifetime:
 * - bx_fetch_scheduler_new() borrows `cfg`; struct bx_fetch_config must outlive BxFetchScheduler.
 * - bx_fetch_scheduler_add_url() canonicalizes an untrusted URL before copying it.
 * - bx_fetch_scheduler_add_canonical_url() is the internal fast path for canonical URLs.
 * - Output path strings are copied into scheduler-owned queue storage.
 */

#include "config.h"

typedef struct BxFetchScheduler BxFetchScheduler;

/*
 * Return true when the scheduler has queued a retry for this attempt and the caller should
 * suppress terminal bookkeeping for the in-flight failure.
 *
 * Callback ownership:
 * - `url` is borrowed from done_userdata-owned transfer info and valid only for
 *   the duration of the callback.
 */
typedef bool (*BxFetchSchedulerTransferDoneFn)(void* userdata, const char* url, int status, int result, bool retryable_hint);
/*
 * Return codes:
 *  0: transfer submitted, scheduler tracks it as active
 * >0: transfer was intentionally skipped/consumed, no retry
 * <0: dispatch error
 *
 * Callback contract:
 * - `url` and `output_path` are borrowed and transient for the call.
 * - If return is 0, dispatch implementation must eventually invoke `on_done()`
 *   exactly once with `done_userdata`.
 */
typedef int (*BxFetchSchedulerDispatchFn)(void* userdata, const char* url, const char* output_path, BxFetchSchedulerTransferDoneFn on_done, void* done_userdata);
/* Called by scheduler while transfers are active to drive completion progress. */
typedef int (*BxFetchSchedulerPollFn)(void* userdata);

BxFetchScheduler* bx_fetch_scheduler_new(const struct bx_fetch_config* cfg, BxFetchSchedulerDispatchFn dispatch, BxFetchSchedulerPollFn poll, void* userdata);
void bx_fetch_scheduler_free(BxFetchScheduler* s);

/* Enqueues one transfer attempt candidate; URL/path are copied on success. */
int bx_fetch_scheduler_add_url(BxFetchScheduler* s, const char* url, const char* output_path);
int bx_fetch_scheduler_add_canonical_url(BxFetchScheduler* s, const char* canonical_url, const char* output_path);
/*
 * Runs dispatch/poll loop until queue and active set are drained.
 * Returns 0 only when no dispatch, transfer, or scheduler invariant failures occurred.
 */
int bx_fetch_scheduler_run(BxFetchScheduler* s);

#endif  // BX_FETCH_SCHEDULER_H
