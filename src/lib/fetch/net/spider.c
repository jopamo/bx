#include "engine_internal.h"
#include "lib/fetch/recovery.h"
#include <errno.h>
#include <stdlib.h>

bool bx_fetch_spider_retry_get(BxFetchTransfer* transfer, int status) {
    BxFetchEngine* engine = transfer->engine;
    if (!bx_fetch_recovery_spider_get(engine->cfg, transfer->req, status, transfer->spider_get))
        return false;

    BxFetchResponse* response = bx_fetch_response_new();
    BxFetchPreparedUrl* target = bx_fetch_prepared_url_clone(bx_fetch_request_target(transfer->req));
    if (!response || !target) {
        bx_fetch_response_free(response);
        bx_fetch_prepared_url_free(target);
        errno = ENOMEM;
        bx_fetch_transfer_mark_io_failure(transfer, ENOMEM);
        return false;
    }
    if (curl_multi_remove_handle(engine->multi, transfer->easy) != CURLM_OK) {
        engine->invariant_failed = true;
        bx_fetch_response_free(response);
        bx_fetch_prepared_url_free(target);
        return false;
    }
    transfer->multi_attached = false;
    /* Restart at the requested URL so libcurl applies its redirect credential
     * rules again. Headers are sufficient; no representation is published. */
    if (curl_easy_setopt(transfer->easy, CURLOPT_URL, bx_fetch_prepared_url_transport(target)) != CURLE_OK ||
        curl_easy_setopt(transfer->easy, CURLOPT_NOBODY, 0L) != CURLE_OK ||
        curl_easy_setopt(transfer->easy, CURLOPT_TIMEOUT_MS, bx_fetch_request_budget_timeout_ms(engine)) != CURLE_OK ||
        curl_easy_setopt(transfer->easy, CURLOPT_CUSTOMREQUEST, "GET") != CURLE_OK ||
        curl_easy_setopt(transfer->easy, CURLOPT_RANGE, "0-0") != CURLE_OK) {
        bx_fetch_response_free(response);
        bx_fetch_prepared_url_free(target);
        errno = EIO;
        bx_fetch_transfer_mark_io_failure(transfer, EIO);
        return false;
    }
    bx_fetch_response_free(transfer->resp);
    transfer->resp = response;
    response->used_spider_get = true;
    bx_fetch_prepared_url_free(transfer->current_target);
    transfer->current_target = target;
    bx_fetch_prepared_url_free(transfer->pending_redirect_target);
    transfer->pending_redirect_target = NULL;
    transfer->response_headers_finalized = false;
    transfer->spider_get = true;
    if (curl_multi_add_handle(engine->multi, transfer->easy) != CURLM_OK) {
        errno = EIO;
        bx_fetch_transfer_mark_io_failure(transfer, EIO);
        return false;
    }
    transfer->multi_attached = true;
    return true;
}
