#ifndef BX_FETCH_RUN_H
#define BX_FETCH_RUN_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, applet */

/*
 * Shared run facade for fetch applets.
 *
 * The facade owns transport, crawl scheduling, and committed publication
 * state. Frontends retain policy: CLI parsing, output naming, header-driven
 * path selection, diagnostics, link extraction decisions, and exit mapping.
 * The immutable config and all callback userdata must outlive the run.
 * Callbacks must not recursively execute, cancel, or free the run.
 * Committed documents are snapshotted into a bounded queue during terminal
 * callbacks and parsed only after the libcurl event-loop call returns.
 */

#include "crawl_coordinator.h"
#include "document.h"
#include "net.h"
#include "publication.h"
#include "transfer_prepare.h"

typedef struct BxFetchRun BxFetchRun;

typedef struct {
    const BxFetchTransferCompletion* transfer;
    int depth;
    int attempt;
    int max_attempts;
    BxFetchPublicationResult publication;
    bool document_queued;
    bool retry_scheduled;
} BxFetchRunCompletion;

typedef void (*BxFetchRunPrepareErrorFn)(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchPrepareError* error);
typedef void (*BxFetchRunSubmitErrorFn)(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchNetSetupError* error);
/*
 * Called after committed-publication accounting and scheduler completion.
 * Returning nonzero stops new work, cancels/drains active transfers, and makes
 * bx_fetch_run_execute() fail. Set errno to preserve a specific cause.
 */
typedef int (*BxFetchRunCompletionFn)(void* userdata, BxFetchRun* run, const BxFetchRunCompletion* completion);
/*
 * shared_decision is authoritative. The callback observes every redirect and
 * may further restrict accepted targets, but cannot override a shared reject.
 */
typedef bool (*BxFetchRunRedirectFn)(void* userdata, const BxFetchPreparedUrl* target, BxFetchFilterDecision shared_decision);
typedef void (*BxFetchRunDiscoveredLinkFn)(void* userdata, const BxFetchPreparedUrl* base, const char* reference, BxFetchHtmlLinkKind kind, int parent_depth, BxFetchCrawlEnqueueResult result);
/*
 * Returns true to continue after a document failure; the frontend then owns
 * any nonzero exit policy. Returning false (or omitting the callback)
 * cancels/drains the run after the current poll.
 */
typedef bool (*BxFetchRunDocumentErrorFn)(void* userdata, const BxFetchPreparedUrl* base, const char* path, int depth, const BxFetchDocumentOutcome* outcome);

typedef struct {
    BxFetchCrawlPlanOutputFn plan_output;
    BxFetchTransferHeadersCallback on_response_headers;
    BxFetchRunPrepareErrorFn on_prepare_error;
    BxFetchRunSubmitErrorFn on_submit_error;
    BxFetchRunCompletionFn on_completion;
    BxFetchRunRedirectFn allow_redirect;
    BxFetchRunDiscoveredLinkFn on_discovered_link;
    BxFetchRunDocumentErrorFn on_document_error;
    BxFetchTransportObserver transport_observer;
    BxFetchSchedulerObserver scheduler_observer;
    void* userdata;
} BxFetchRunFrontend;

/*
 * libcurl global state must already be initialized unless dry-run is enabled.
 * callbacks are copied; cfg and callback userdata are borrowed.
 */
BxFetchRun* bx_fetch_run_new(const struct bx_fetch_config* cfg, const BxFetchRunFrontend* frontend);
/*
 * Cancels and drains owned transfers before releasing scheduler state.
 * Cancellation can invoke terminal frontend callbacks.
 */
void bx_fetch_run_free(BxFetchRun* run);

BxFetchCrawlEnqueueResult bx_fetch_run_add_seed(BxFetchRun* run, const char* url);
BxFetchCrawlEnqueueResult bx_fetch_run_add_discovered(BxFetchRun* run, const BxFetchPreparedUrl* base, const char* reference, BxFetchHtmlLinkKind kind, int parent_depth);

int bx_fetch_run_execute(BxFetchRun* run);
void bx_fetch_run_cancel(BxFetchRun* run);
BxFetchCrawlPhase bx_fetch_run_phase(const BxFetchRun* run);

/*
 * Persistence remains explicit so a frontend can place link conversion at the
 * correct point in its lifecycle. The underlying policy is config-gated.
 */
int bx_fetch_run_load_publication(BxFetchRun* run);
int bx_fetch_run_save_publication(const BxFetchRun* run);
const BxFetchPublicationState* bx_fetch_run_publication(const BxFetchRun* run);

#endif  // BX_FETCH_RUN_H
