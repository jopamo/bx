#ifndef BX_FETCH_NET_H
#define BX_FETCH_NET_H

/* BX_FETCH_HEADER_OWNER: net */
/* BX_FETCH_HEADER_CONSUMERS: net, core, applet */

/*
 * BxFetchEngine is the single-threaded libcurl-multi transport shared by all
 * fetch frontends. It owns successfully submitted request/writer pairs until
 * one terminal callback has run. Presentation is injected once at engine
 * construction through typed observations; transport code never formats
 * applet diagnostics.
 */

#include "config.h"
#include "error.h"
#include "request.h"
#include "response.h"
#include "writer.h"
#include <stddef.h>
#include <stdint.h>

typedef struct BxFetchEngine BxFetchEngine;

typedef struct {
    bool present;
    const char* setting;
    const char* capability;
    const char* option_name;
    const char* detail;
    int curl_code;
    int error_number;
} BxFetchNetSetupError;

typedef struct {
    bool total_known;
    int percent;
    int64_t downloaded_bytes;
    int64_t total_bytes;
    double bytes_per_second;
    double elapsed_seconds;
    double eta_seconds;
} BxFetchProgress;

/*
 * All values passed to observation callbacks are borrowed and valid only for
 * the callback. raw_header is bounded by BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES
 * and includes the original line ending.
 */
typedef struct {
    void (*on_response_header)(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, const char* raw_header, size_t raw_header_len);
    void (*on_progress)(void* userdata, const BxFetchRequest* request, const BxFetchProgress* progress);
    void* userdata;
} BxFetchTransportObserver;

/*
 * Process-global libcurl lifetime. The caller must initialize once before
 * constructing engines and clean up only after every engine is freed.
 * Capability validation is fail-closed and reports typed detail without
 * printing frontend-specific text.
 */
int bx_fetch_global_init(const struct bx_fetch_config* cfg, BxFetchNetSetupError* setup_error);
void bx_fetch_global_cleanup(void);

/*
 * Called at most once for commit-eligible HTTP 200/206 headers. writer is
 * borrowed and must not be consumed by the callback.
 */
typedef int (*BxFetchTransferHeadersCallback)(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer);

/*
 * Called exactly once for every successful submission, including explicit
 * cancellation and engine teardown. All pointers are borrowed and become
 * invalid after the callback returns.
 */
typedef void (*BxFetchTransferCallback)(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchError result);

/*
 * Receives an already resolved and canonical redirect target. Return false to
 * reject it. The callback is the authoritative frontend/core redirect policy;
 * the transport independently preserves protocol and credential constraints.
 */
typedef bool (*BxFetchRedirectPolicyCallback)(void* userdata, const char* redirect_url);

/*
 * cfg and observer->userdata are borrowed and must outlive the engine. The
 * observer table itself is copied and may be temporary.
 */
BxFetchEngine* bx_fetch_engine_new(const struct bx_fetch_config* cfg, const BxFetchTransportObserver* observer);
/*
 * Cancels active work, aborts each candidate writer, invokes each terminal
 * callback once with BX_FETCH_ERROR_CANCELLED, and rejects later submissions.
 */
void bx_fetch_engine_cancel(BxFetchEngine* engine);
/* Cancels any remaining work before releasing engine resources. */
void bx_fetch_engine_free(BxFetchEngine* engine);

/*
 * On success, the engine owns req and writer. On failure, ownership remains
 * with the caller. A successfully submitted pair reaches exactly one terminal
 * callback.
 */
int bx_fetch_engine_submit(BxFetchEngine* engine,
                           BxFetchRequest* req,
                           BxFetchWriter* writer,
                           BxFetchTransferHeadersCallback headers_cb,
                           BxFetchTransferCallback callback,
                           void* userdata,
                           BxFetchRedirectPolicyCallback redirect_cb,
                           void* redirect_userdata);

int bx_fetch_engine_submit_with_setup_error(BxFetchEngine* engine,
                                            BxFetchRequest* req,
                                            BxFetchWriter* writer,
                                            BxFetchTransferHeadersCallback headers_cb,
                                            BxFetchTransferCallback callback,
                                            void* userdata,
                                            BxFetchRedirectPolicyCallback redirect_cb,
                                            void* redirect_userdata,
                                            BxFetchNetSetupError* setup_error);

/*
 * Drives one event-loop iteration, blocking for at most about 100 ms while
 * active. Returns -1 only for an engine/runtime invariant failure.
 */
int bx_fetch_engine_run(BxFetchEngine* engine);
bool bx_fetch_engine_is_active(const BxFetchEngine* engine);
bool bx_fetch_engine_quota_exhausted(const BxFetchEngine* engine);

#endif  // BX_FETCH_NET_H
