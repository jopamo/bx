#ifndef BX_FETCH_NET_ENGINE_INTERNAL_H
#define BX_FETCH_NET_ENGINE_INTERNAL_H

#include "lib/fetch/net.h"
#include "lib/fetch/rate_limiter.h"
#include "transfer.h"
#include <curl/curl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct BxFetchEngine {
    const struct bx_fetch_config* cfg;
    BxFetchTransportObserver observer;
    CURLM* multi;
    int epoll_fd;
    int timer_fd;
    size_t active_transfers;
    BxFetchTransfer* active_head;
    int64_t quota_limit_bytes;
    uint64_t downloaded_bytes;
    bool quota_exhausted;
    bool invariant_failed;
    bool cancelled;
    BxFetchTokenBucket rate_limiter;
};

void bx_fetch_engine_detach_transfer(BxFetchEngine* engine, BxFetchTransfer* target);
bool bx_fetch_net_require(BxFetchEngine* engine, bool condition);
bool bx_fetch_transfer_abort_writer(BxFetchTransfer* transfer);
int bx_fetch_transfer_close_writer(BxFetchTransfer* transfer);
int bx_fetch_transfer_close_writer_metadata_only(BxFetchTransfer* transfer);
void bx_fetch_engine_dispose_transfer(BxFetchEngine* engine, BxFetchTransfer* transfer, BxFetchError result);
double bx_fetch_monotonic_seconds(void);
int bx_fetch_sleep_for_seconds(double seconds);
void bx_fetch_record_downloaded_bytes(BxFetchTransfer* transfer, size_t total);
void bx_fetch_transfer_mark_io_failure(BxFetchTransfer* transfer, int fallback_error_number);
void bx_fetch_response_reset_headers(BxFetchResponse* response);
bool bx_fetch_parse_resume_from_request(const BxFetchRequest* request, long long* resume_from);

void bx_fetch_progress_emit(BxFetchTransfer* transfer, curl_off_t download_total, curl_off_t downloaded);
int bx_fetch_progress_callback(void* userdata, curl_off_t download_total, curl_off_t downloaded, curl_off_t upload_total, curl_off_t uploaded);

BxFetchError bx_fetch_map_curl_result(CURLcode code);
BxFetchTransportErrorKind bx_fetch_classify_curl_transport_error(CURLcode code);
size_t bx_fetch_header_callback(char* data, size_t size, size_t count, void* userdata);
size_t bx_fetch_write_callback(char* data, size_t size, size_t count, void* userdata);

#endif  // BX_FETCH_NET_ENGINE_INTERNAL_H
