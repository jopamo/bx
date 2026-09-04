#define _GNU_SOURCE
#include "engine_internal.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

void bx_fetch_engine_detach_transfer(BxFetchEngine* engine, BxFetchTransfer* target) {
    if (!engine || !target)
        return;

    BxFetchTransfer** link = &engine->active_head;
    while (*link && *link != target)
        link = &(*link)->next_active;

    if (!*link) {
        engine->invariant_failed = true;
        errno = EPROTO;
        return;
    }

    *link = target->next_active;
    target->next_active = NULL;
    if (engine->active_transfers == 0) {
        engine->invariant_failed = true;
        errno = EPROTO;
        return;
    }
    engine->active_transfers--;
}

bool bx_fetch_net_require(BxFetchEngine* engine, bool condition) {
    if (condition)
        return true;
    if (engine)
        engine->invariant_failed = true;
    errno = EPROTO;
    return false;
}

bool bx_fetch_transfer_abort_writer(BxFetchTransfer* transfer) {
    if (!transfer)
        return false;
    if (transfer->writer_aborted || transfer->writer_closed)
        return transfer->writer == NULL;
    if (!transfer->writer)
        return false;

    bx_fetch_writer_abort(transfer->writer);
    transfer->writer = NULL;
    transfer->writer_aborted = true;
    if (transfer->resp && transfer->resp->output_state == BX_FETCH_OUTPUT_STATE_NONE)
        transfer->resp->output_state = BX_FETCH_OUTPUT_STATE_ABORTED;
    return true;
}

int bx_fetch_transfer_close_writer(BxFetchTransfer* transfer) {
    if (!transfer || transfer->writer_closed || transfer->writer_aborted || !transfer->writer) {
        errno = EINVAL;
        return -1;
    }

    int result = bx_fetch_writer_close(transfer->writer);
    transfer->writer = NULL;
    transfer->writer_closed = true;
    if (transfer->resp)
        transfer->resp->output_state = result == 0 ? BX_FETCH_OUTPUT_STATE_COMMITTED : BX_FETCH_OUTPUT_STATE_COMMIT_FAILED;
    return result;
}

void bx_fetch_engine_dispose_transfer(BxFetchEngine* engine, BxFetchTransfer* transfer, BxFetchError result) {
    if (!engine || !transfer)
        return;

    /*
     * Remove active ownership before invoking frontend code. A callback may
     * inspect engine state, but must never observe this transfer as active
     * after its terminal result is visible.
     */
    if (engine->multi && transfer->easy) {
        CURLMcode remove_result = curl_multi_remove_handle(engine->multi, transfer->easy);
        bx_fetch_net_require(engine, remove_result == CURLM_OK);
    }
    bx_fetch_engine_detach_transfer(engine, transfer);

    if (transfer->writer && !bx_fetch_transfer_abort_writer(transfer)) {
        engine->invariant_failed = true;
        result = BX_FETCH_ERROR_INTERNAL;
    }
    if (!bx_fetch_net_require(engine, transfer->writer == NULL))
        result = BX_FETCH_ERROR_INTERNAL;

    transfer->state = result == BX_FETCH_OK ? BX_FETCH_TRANSFER_STATE_COMPLETED : BX_FETCH_TRANSFER_STATE_FAILED;

    if (!transfer->terminal_callback_invoked) {
        transfer->terminal_callback_invoked = true;
        if (transfer->callback)
            transfer->callback(transfer->callback_userdata, transfer->req, transfer->resp, result);
    }
    else {
        engine->invariant_failed = true;
    }

    bx_fetch_transfer_free(transfer);
}

double bx_fetch_monotonic_seconds(void) {
    struct timespec timestamp;
    if (clock_gettime(CLOCK_MONOTONIC, &timestamp) != 0)
        return 0.0;
    return (double)timestamp.tv_sec + ((double)timestamp.tv_nsec / 1000000000.0);
}

int bx_fetch_sleep_for_seconds(double seconds) {
    if (seconds <= 0.0)
        return 0;

    struct timespec delay = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = (long)((seconds - (double)((time_t)seconds)) * 1000000000.0),
    };
    if (delay.tv_nsec < 0)
        delay.tv_nsec = 0;

    while (nanosleep(&delay, &delay) == -1) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

void bx_fetch_record_downloaded_bytes(BxFetchTransfer* transfer, size_t total) {
    if (!transfer || !transfer->engine || transfer->engine->quota_limit_bytes < 0) {
        return;
    }

    BxFetchEngine* engine = transfer->engine;
    if ((uint64_t)total > UINT64_MAX - engine->downloaded_bytes)
        engine->downloaded_bytes = UINT64_MAX;
    else
        engine->downloaded_bytes += (uint64_t)total;

    if (engine->downloaded_bytes >= (uint64_t)engine->quota_limit_bytes)
        engine->quota_exhausted = true;
}

void bx_fetch_transfer_mark_io_failure(BxFetchTransfer* transfer, int fallback_error_number) {
    if (!transfer)
        return;

    transfer->io_failed = true;
    int error_number = errno > 0 ? errno : fallback_error_number;
    if (error_number > 0 && transfer->io_error_number <= 0)
        transfer->io_error_number = error_number;
}

void bx_fetch_response_reset_headers(BxFetchResponse* response) {
    if (!response)
        return;

    for (size_t i = 0; i < response->header_count; i++) {
        free(response->headers[i].name);
        free(response->headers[i].value);
    }
    free(response->headers);
    response->headers = NULL;
    response->header_count = 0;
    response->header_capacity = 0;
    response->header_bytes = 0;
}

bool bx_fetch_parse_resume_from_request(const BxFetchRequest* request, long long* resume_from) {
    if (!request || !resume_from)
        return false;

    for (size_t i = 0; i < request->header_count; i++) {
        if (!request->headers[i].name || !request->headers[i].value || strcasecmp(request->headers[i].name, "Range") != 0) {
            continue;
        }

        const char* value = request->headers[i].value;
        if (strncasecmp(value, "bytes=", 6) != 0)
            continue;

        char* end = NULL;
        long long parsed = strtoll(value + 6, &end, 10);
        if (end != value + 6 && parsed >= 0 && end && *end == '-') {
            *resume_from = parsed;
            return true;
        }
    }
    return false;
}
