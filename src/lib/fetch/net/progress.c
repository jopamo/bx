#define _GNU_SOURCE
#include "engine_internal.h"

void bx_fetch_progress_emit(BxFetchTransfer* transfer, curl_off_t download_total, bool authoritative_length) {
    /* easy cleanup may call back (notably FTP). Completion releases the
     * submission userdata, so it is also the observation lifetime boundary. */
    if (!transfer || !transfer->progress_cb || transfer->terminal_callback_invoked)
        return;

    BxFetchProtocol protocol = bx_fetch_prepared_url_protocol(transfer->current_target);
    bool http = protocol == BX_FETCH_PROTOCOL_HTTP || protocol == BX_FETCH_PROTOCOL_HTTPS;
    /*
     * libcurl counters can still describe the previous response at a redirect
     * boundary. Body callbacks, unlike XFERINFO, belong to our current response.
     * A partial response's total comes only from validated Content-Range.
     */
    if ((!http || transfer->response_headers_finalized) &&
        transfer->resp->status_code != 206 && !transfer->resume_validation_failed &&
        (download_total > 0 || (authoritative_length && download_total == 0))) {
        transfer->progress.total_known = true;
        transfer->progress.total_bytes = (uint64_t)download_total;
    }
    transfer->progress.received_bytes = (uint64_t)transfer->response_body_bytes;
    BxFetchProgressSample sample = transfer->progress;
    if (http && (!transfer->response_headers_finalized || transfer->resume_validation_failed)) {
        sample.accepted_prefix_bytes = 0;
        sample.total_known = false;
        sample.total_bytes = 0;
    }
    transfer->progress_cb(transfer->callback_userdata, transfer->req, &sample);
    transfer->progress_emitted = true;
}

int bx_fetch_progress_callback(void* userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded) {
    BxFetchTransfer* transfer = userdata;
    (void)downloaded;
    (void)upload_total;
    (void)uploaded;
    if (!transfer || transfer->terminal_callback_invoked)
        return 0;
    if (!bx_fetch_request_budget_check(transfer))
        return 1;

    double now = bx_fetch_monotonic_seconds();
    if (!transfer->progress_emitted || now - transfer->progress_last_update_s >= 0.125) {
        /* XFERINFO zero means unknown, never a known empty object. */
        bx_fetch_progress_emit(transfer, download_total, false);
        transfer->progress_last_update_s = now;
    }
    return transfer->engine && transfer->engine->cancelled ? 1 : 0;
}
