#ifndef BX_FETCH_NET_TRANSFER_H
#define BX_FETCH_NET_TRANSFER_H

#include "lib/fetch/anubis.h"
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

typedef enum {
    BX_FETCH_ANUBIS_PHASE_NONE = 0,
    BX_FETCH_ANUBIS_PHASE_SUBMIT,
    BX_FETCH_ANUBIS_PHASE_RETRY,
} BxFetchAnubisPhase;

typedef struct BxFetchTransfer {
    CURL* easy;
    BxFetchRequest* req;
    BxFetchResponse* resp;
    BxFetchWriter* writer;
    BxFetchEngine* engine;
    struct BxFetchTransfer* next_active;
    struct curl_slist* headers;
    BxFetchTransferState state;
    bool multi_attached;

    bool resume_requested;
    long long resume_from;
    bool resume_needs_content_range;
    bool resume_saw_content_range;
    bool discard_body;
    bool resume_validation_failed;
    bool resume_restart_validation_pending;
    bool io_failed;
    bool downstream_closed;
    bool request_body_io_failed;
    int io_error_number;
    curl_off_t response_body_bytes;
    size_t response_header_bytes;
    char* transform_source;
    size_t transform_source_len;
    size_t transform_source_cap;
    bool transform_failed;

    bool save_headers_written;
    char* save_headers_buf;
    size_t save_headers_len;
    size_t save_headers_cap;

    BxFetchProgressSample progress;
    bool progress_emitted;
    double progress_last_update_s;

    bool response_headers_finalized;
    bool writer_closed;
    bool writer_aborted;
    bool terminal_callback_invoked;

    char* anubis_probe;
    size_t anubis_probe_len;
    bool anubis_probe_active;
    bool anubis_challenge_detected;
    bool anubis_refresh_detected;
    BxFetchAnubisChallenge anubis_challenge;
    BxFetchAnubisPhase anubis_phase;
    int anubis_error_number;
    const char* anubis_error_detail;
    BxFetchPreparedUrl* anubis_retry_target;

    BxFetchPreparedUrl* current_target;
    BxFetchPreparedUrl* pending_redirect_target;
    bool redirect_policy_rejected;
    bool url_canonicalization_failed;
    bool redirect_protocol_unsupported;
    BxFetchNetTargetPolicy redirect_target_policy;

    BxFetchTransferHeadersCallback headers_cb;
    BxFetchTransferProgressCallback progress_cb;
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
