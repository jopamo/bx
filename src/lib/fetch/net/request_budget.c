#include "engine_internal.h"
#include <limits.h>
#include <errno.h>

int bx_fetch_budget_init(BxFetchBudget* budget, int max_requests, int max_seconds) {
    double now = bx_fetch_monotonic_seconds();
    if (!budget || max_requests < 0 || max_seconds < 0 || now <= 0) {
        errno = EINVAL;
        return -1;
    }
    *budget = (BxFetchBudget){.max_requests = max_requests, .started_at = now,
        .deadline = max_seconds > 0 ? now + max_seconds : 0};
    return 0;
}

uint64_t bx_fetch_budget_elapsed_ms(const BxFetchBudget* budget) {
    double elapsed = (bx_fetch_monotonic_seconds() - budget->started_at) * 1000.0;
    return elapsed <= 0 ? 0 : elapsed >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)elapsed;
}

bool bx_fetch_engine_time_exhausted(const BxFetchEngine* engine) {
    if (!engine || engine->budget->deadline <= 0)
        return false;
    double now = bx_fetch_monotonic_seconds();
    return now <= 0 || now >= engine->budget->deadline;
}

bool bx_fetch_request_budget_check(BxFetchTransfer* transfer) {
    if (bx_fetch_engine_time_exhausted(transfer->engine))
        transfer->time_budget_exhausted = true;
    return !transfer->time_budget_exhausted;
}

long bx_fetch_request_budget_timeout_ms(BxFetchEngine* engine) {
    if (engine->budget->deadline <= 0)
        return 0;
    double remaining = (engine->budget->deadline - bx_fetch_monotonic_seconds()) * 1000.0;
    if (remaining <= 1)
        return 1;
    return remaining >= (double)LONG_MAX ? LONG_MAX : (long)remaining + 1;
}

int bx_fetch_request_budget_callback(void* userdata, char* primary_ip,
                                     char* local_ip, int primary_port, int local_port) {
    BxFetchTransfer* transfer = userdata;
    (void)primary_ip;
    (void)local_ip;
    (void)primary_port;
    (void)local_port;
    BxFetchEngine* engine = transfer->engine;
    if (!bx_fetch_request_budget_check(transfer))
        return 1;
    if (engine->budget->max_requests > 0 &&
        engine->budget->requests_started >= (uint64_t)engine->budget->max_requests) {
        transfer->request_budget_exhausted = true;
        return 1;
    }
    /* libcurl calls this before sending a request, including redirects and
     * authentication exchanges. Connection failures consume no request slot. */
    if (engine->budget->requests_started < UINT64_MAX)
        engine->budget->requests_started++;
    return 0;
}
