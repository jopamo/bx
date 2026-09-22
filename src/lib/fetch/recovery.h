#ifndef BX_FETCH_RECOVERY_H
#define BX_FETCH_RECOVERY_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, net, applet */

#include "config.h"
#include "error.h"
#include "request.h"
#include "response.h"
#include <stdint.h>

typedef enum {
    BX_FETCH_RECOVERY_SUCCESS = 0,
    BX_FETCH_RECOVERY_TERMINAL,
    BX_FETCH_RECOVERY_RETRY,
    BX_FETCH_RECOVERY_ATTEMPTS_EXHAUSTED,
    BX_FETCH_RECOVERY_TIME_EXHAUSTED,
    BX_FETCH_RECOVERY_REQUESTS_EXHAUSTED,
    BX_FETCH_RECOVERY_SCHEDULER_STOPPED,
} BxFetchRecoveryReason;

typedef struct {
    BxFetchRecoveryReason reason;
    int delay_seconds;
} BxFetchRecoveryDecision;

bool bx_fetch_recovery_retryable_hint(const struct bx_fetch_config* cfg,
                                      const BxFetchRequest* request,
                                      const BxFetchResponse* response, BxFetchError result);
bool bx_fetch_recovery_spider_get(const struct bx_fetch_config* cfg, const BxFetchRequest* request, int status, bool already_used);

/*
 * Pure retry policy. The hint must already exclude unsafe method replay and
 * irreversible output. Elapsed time includes work and waits since run start.
 * Retry-After is a minimum delay, never shortened to fit a budget.
 * RETRY is a proposal here; scheduler completion returns it only after enqueue.
 */
BxFetchRecoveryDecision bx_fetch_recovery_plan(const struct bx_fetch_config* cfg,
                                              int status, BxFetchError result,
                                              bool retryable_hint, int attempt,
                                              int64_t retry_after_seconds,
                                              double elapsed_seconds);

#endif
