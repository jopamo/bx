#include "lib/fetch/recovery.h"
#include "lib/fetch/http_status.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <string.h>

enum { BX_FETCH_CURL_CODE_COULDNT_CONNECT = 7 };

bool bx_fetch_recovery_spider_get(const struct bx_fetch_config* cfg, const BxFetchRequest* request, int status, bool already_used) {
    return cfg && cfg->download.spider && cfg->download.spider_get_fallback &&
           !already_used && (status == 405 || status == 501) &&
           request && request->method &&
           (strcmp(request->method, "GET") == 0 || strcmp(request->method, "HEAD") == 0) &&
           !request->body_len && !request->body_file;
}

static bool retry_http_status(const struct bx_fetch_config* cfg, int status, bool rate_limited) {
    if (cfg->download.retry_on_http_error)
        return bx_fetch_http_status_list_contains(cfg->download.retry_on_http_error, status);
    if (!cfg->download.retry_transient_http)
        return false;
    return rate_limited || status == 408 || status == 425 || status == 429 ||
           status == 500 || status == 502 || status == 503 || status == 504;
}

BxFetchRecoveryDecision bx_fetch_recovery_plan(const struct bx_fetch_config* cfg,
                                              int status, BxFetchError result,
                                              bool retryable_hint, int attempt,
                                              int64_t retry_after_seconds,
                                              double elapsed_seconds) {
    BxFetchRecoveryDecision decision = {.reason = BX_FETCH_RECOVERY_TERMINAL};
    if (!cfg || attempt < 1 || !isfinite(elapsed_seconds) || elapsed_seconds < 0)
        return decision;
    if (result == BX_FETCH_OK) {
        decision.reason = BX_FETCH_RECOVERY_SUCCESS;
        return decision;
    }
    if (result == BX_FETCH_ERROR_TIME_BUDGET || result == BX_FETCH_ERROR_REQUEST_BUDGET) {
        decision.reason = result == BX_FETCH_ERROR_TIME_BUDGET
            ? BX_FETCH_RECOVERY_TIME_EXHAUSTED : BX_FETCH_RECOVERY_REQUESTS_EXHAUSTED;
        return decision;
    }
    /* Rejected authentication and permissions remain terminal, including
     * when transport also fails while receiving the error response. */
    if (status == 401 || (status == 403 && result != BX_FETCH_ERROR_RATE_LIMIT) || status == 407 ||
        !retryable_hint || result == BX_FETCH_ERROR_CANCELLED ||
        result == BX_FETCH_ERROR_UNSUPPORTED || result == BX_FETCH_ERROR_RESOURCE_LIMIT ||
        ((result == BX_FETCH_ERROR_HTTP || result == BX_FETCH_ERROR_RATE_LIMIT) &&
         !retry_http_status(cfg, status, result == BX_FETCH_ERROR_RATE_LIMIT)))
        return decision;
    if (attempt >= cfg->download.tries) {
        decision.reason = BX_FETCH_RECOVERY_ATTEMPTS_EXHAUSTED;
        return decision;
    }
    int delay = attempt < cfg->download.waitretry ? attempt : cfg->download.waitretry;
    if (delay < 0)
        return decision;
    int64_t required_delay = retry_after_seconds > delay ? retry_after_seconds : delay;
    if (required_delay > INT_MAX ||
        (cfg->download.max_retry_time > 0 &&
         elapsed_seconds + (double)required_delay >= cfg->download.max_retry_time)) {
        decision.reason = BX_FETCH_RECOVERY_TIME_EXHAUSTED;
        return decision;
    }
    decision.reason = BX_FETCH_RECOVERY_RETRY;
    decision.delay_seconds = (int)required_delay;
    return decision;
}

static bool retryable_io_error_number(int error_number) {
    if (error_number < 0)
        return true;

    switch (error_number) {
        case EINTR:
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case ETIMEDOUT:
        case ENFILE:
        case EMFILE:
            return true;
        default:
            return false;
    }
}

bool bx_fetch_recovery_retryable_hint(const struct bx_fetch_config* cfg, const BxFetchRequest* request,
                                       const BxFetchResponse* response, BxFetchError result) {
    if (!request || !request->method ||
        (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) ||
        request->body_len || request->body_file ||
        (response && response->output_state == BX_FETCH_OUTPUT_STATE_COMMIT_FAILED))
        return false;
    int curl_code = response ? response->error_code : 0;
    int error_number = response ? response->error_number : -1;
    BxFetchTransportErrorKind transport_kind = response ? response->transport_error_kind : BX_FETCH_TRANSPORT_ERROR_NONE;

    switch (result) {
        case BX_FETCH_ERROR_SSL:
            return transport_kind == BX_FETCH_TRANSPORT_ERROR_TLS_RETRYABLE;
        case BX_FETCH_ERROR_NETWORK:
            if (transport_kind == BX_FETCH_TRANSPORT_ERROR_AUTH)
                return false;
            if (transport_kind == BX_FETCH_TRANSPORT_ERROR_SERVER)
                return response && response->status_code >= 400 && response->status_code < 500;
            if (error_number == ECONNREFUSED || curl_code == BX_FETCH_CURL_CODE_COULDNT_CONNECT)
                return cfg && cfg->download.retry_connrefused;
            return true;
        case BX_FETCH_ERROR_IO:
            return retryable_io_error_number(error_number);
        case BX_FETCH_ERROR_MEMORY:
        case BX_FETCH_ERROR_INVALID_ARGUMENT:
        case BX_FETCH_ERROR_UNSUPPORTED:
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
        case BX_FETCH_ERROR_INTERNAL:
            return false;
        case BX_FETCH_ERROR_HTTP:
        case BX_FETCH_ERROR_RATE_LIMIT:
        case BX_FETCH_ERROR_TIMEOUT:
            return true;
        default:
            return false;
    }
}
