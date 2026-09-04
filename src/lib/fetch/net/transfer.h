#ifndef BX_FETCH_NET_TRANSFER_H
#define BX_FETCH_NET_TRANSFER_H

#include "lib/fetch/net.h"
#include <stdbool.h>
#include <stddef.h>
#include <curl/curl.h>

typedef struct BxFetchEngine BxFetchEngine;

typedef enum {
    BX_FETCH_TRANSFER_STATE_INIT,
    BX_FETCH_TRANSFER_STATE_ONGOING,
    BX_FETCH_TRANSFER_STATE_COMPLETED,
    BX_FETCH_TRANSFER_STATE_FAILED,
} BxFetchTransferState;

typedef struct BxFetchTransfer {
    CURL* easy;
    BxFetchRequest* req;
    BxFetchResponse* resp;
    BxFetchWriter* writer;
    BxFetchEngine* engine;
    struct BxFetchTransfer* next_active;
    struct curl_slist* headers;
    BxFetchTransferState state;
    int retry_count;

    bool resume_requested;
    long long resume_from;
    bool resume_needs_content_range;
    bool resume_saw_content_range;
    bool discard_body;
    bool resume_validation_failed;
    bool resume_restart_validation_pending;
    bool io_failed;
    bool request_body_io_failed;
    int io_error_number;
    int resume_status_code;
    curl_off_t response_body_bytes;
    size_t response_header_bytes;

    bool save_headers_written;
    char* save_headers_buf;
    size_t save_headers_len;
    size_t save_headers_cap;

    bool progress_started;
    double progress_start_s;
    double progress_last_update_s;
    double progress_last_sample_s;
    double progress_last_speed_bps;
    curl_off_t progress_last_bytes;
    curl_off_t progress_resume_offset;

    bool response_headers_finalized;
    bool writer_closed;
    bool writer_aborted;
    bool terminal_callback_invoked;

    char* current_url;
    char* pending_redirect_url;
    bool redirect_policy_rejected;
    bool url_canonicalization_failed;
    bool redirect_protocol_unsupported;

    BxFetchTransferHeadersCallback headers_cb;
    BxFetchTransferCallback callback;
    void* callback_userdata;
    BxFetchRedirectPolicyCallback redirect_cb;
    void* redirect_userdata;
} BxFetchTransfer;

/*
 * Internal constructor. It borrows both arguments until engine submission
 * succeeds. The engine then owns request and writer; every terminal path must
 * consume the writer through close/abort before bx_fetch_transfer_free().
 */
BxFetchTransfer* bx_fetch_transfer_new(BxFetchRequest* req, BxFetchWriter* writer);
void bx_fetch_transfer_free(BxFetchTransfer* t);

#endif  // BX_FETCH_NET_TRANSFER_H
