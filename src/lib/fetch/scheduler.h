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
 * - Every queued/in-flight attempt owns immutable prepared URL state.
 * - bx_fetch_scheduler_add_url() normalizes an untrusted URL once.
 * - Prepared/canonical entry points clone or prepare without reparsing in the
 *   retry path.
 * - Output path strings and crawl depth are copied into scheduler-owned queue
 *   storage and remain attached through retries.
 */

#include "config.h"
#include "error.h"
#include "url.h"

typedef struct BxFetchScheduler BxFetchScheduler;

/*
 * Return true when the scheduler has queued a retry for this attempt and the caller should
 * suppress terminal bookkeeping for the in-flight failure.
 *
 * done_userdata remains valid until the callback returns.
 */
typedef bool (*BxFetchSchedulerTransferDoneFn)(void* userdata, int status, BxFetchError result, bool retryable_hint);
/*
 * Return codes:
 *  0: transfer submitted, scheduler tracks it as active
 * >0: transfer was intentionally skipped/consumed, no retry
 * <0: dispatch error
 *
 * Callback contract:
 * - `target` and `output_path` are borrowed and transient for the call.
 * - If return is 0, dispatch implementation must eventually invoke `on_done()`
 *   exactly once with `done_userdata`.
 * - A nonzero return must not invoke `on_done()`.
 */
typedef int (*BxFetchSchedulerDispatchFn)(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, int depth, BxFetchSchedulerTransferDoneFn on_done, void* done_userdata);
/* Called by scheduler while transfers are active to drive completion progress. */
typedef int (*BxFetchSchedulerPollFn)(void* userdata);

typedef struct {
    /*
     * Values are borrowed for the callback. Observers must not mutate or free
     * the scheduler.
     */
    void (*on_retry)(void* userdata, const BxFetchPreparedUrl* target, int next_attempt, int max_attempts, int delay_seconds);
    void* userdata;
} BxFetchSchedulerObserver;

BxFetchScheduler* bx_fetch_scheduler_new(const struct bx_fetch_config* cfg, BxFetchSchedulerDispatchFn dispatch, BxFetchSchedulerPollFn poll, void* userdata, const BxFetchSchedulerObserver* observer);
/* Requires all successfully dispatched work to have completed. */
void bx_fetch_scheduler_free(BxFetchScheduler* s);

/* Enqueues one transfer attempt candidate; URL/path are copied on success. */
int bx_fetch_scheduler_add_url(BxFetchScheduler* s, const char* url, const char* output_path, int depth);
int bx_fetch_scheduler_add_canonical_url(BxFetchScheduler* s, const char* canonical_url, const char* output_path, int depth);
int bx_fetch_scheduler_add_prepared_url(BxFetchScheduler* s, const BxFetchPreparedUrl* target, const char* output_path, int depth);
/*
 * Permanently stops new dispatch/retry work and drops queued candidates.
 * In-flight work remains owned by the dispatch implementation and must still
 * complete exactly once; run() continues polling until those callbacks drain.
 */
void bx_fetch_scheduler_cancel(BxFetchScheduler* s);
bool bx_fetch_scheduler_was_cancelled(const BxFetchScheduler* s);
/*
 * Runs dispatch/poll loop until queue and active set are drained.
 * Returns 0 only when no dispatch, transfer, or scheduler invariant failures occurred.
 */
int bx_fetch_scheduler_run(BxFetchScheduler* s);

#endif  // BX_FETCH_SCHEDULER_H
