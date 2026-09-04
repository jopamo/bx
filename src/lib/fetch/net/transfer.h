#ifndef BX_FETCH_NET_TRANSFER_H
#define BX_FETCH_NET_TRANSFER_H

#include "lib/fetch/request.h"
#include "lib/fetch/response.h"
#include "lib/fetch/writer.h"
#include <stdbool.h>
#include <stddef.h>
#include <curl/curl.h>

typedef struct NetEngine NetEngine;

typedef enum {
    TRANSFER_STATE_INIT,
    TRANSFER_STATE_ONGOING,
    TRANSFER_STATE_COMPLETED,
    TRANSFER_STATE_FAILED,
} BxFetchTransferState;

typedef struct BxFetchTransfer {
    CURL* easy;
    BxFetchRequest* req;
    BxFetchResponse* resp;
    BxFetchWriter* writer;
    NetEngine* engine;
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
    bool progress_line_active;
    double progress_start_s;
    double progress_last_update_s;
    double progress_last_sample_s;
    double progress_last_speed_bps;
    curl_off_t progress_last_bytes;
    size_t progress_last_line_len;
    curl_off_t progress_resume_offset;

    bool wget_basic_output;
    bool wget_connection_reported;
    bool wget_headers_reported;
    bool wget_progress_emitted;
    bool response_headers_finalized;
    bool writer_closed;
    bool writer_aborted;
    bool terminal_callback_invoked;

    char* current_url;
    char* pending_redirect_url;
    bool redirect_policy_rejected;
    bool url_canonicalization_failed;
    bool redirect_protocol_unsupported;

    // Callback for when transfer is complete
    void (*on_complete)(void* userdata, int result);
    void* userdata;
} BxFetchTransfer;

BxFetchTransfer* bx_fetch_transfer_new(BxFetchRequest* req, BxFetchWriter* writer);
void bx_fetch_transfer_free(BxFetchTransfer* t);

#endif  // BX_FETCH_NET_TRANSFER_H
