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
#include "input.h"
#include "link_conversion.h"
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
/*
 * Observes every eligible downloaded file. On failure, true continues with
 * later files and leaves exit policy to the frontend; false stops conversion.
 * The return value is ignored for successful/unchanged/skipped outcomes.
 */
typedef bool (*BxFetchRunLinkConversionFn)(void* userdata, const BxFetchDownloadedFileView* download, const BxFetchLinkConversionOutcome* outcome);

typedef struct {
    int index;
    BxFetchCrawlEnqueueResult result;
    int error_number;
    /* Borrowed source path for input-file seeds; NULL for direct operands. */
    const char* source_path;
    /* Borrowed normalized target; NULL only when URL preparation failed. */
    const BxFetchPreparedUrl* target;
} BxFetchRunSeedObservation;

/*
 * Observes every configured input URL after shared preparation, filtering, and
 * deduplication. Returning true after a policy-rejected result continues with
 * later inputs and leaves aggregate exit policy to the frontend. Internal
 * errors always stop the session; the return value is ignored for enqueued,
 * duplicate, and error results.
 */
typedef bool (*BxFetchRunSeedResultFn)(void* userdata, const BxFetchRunSeedObservation* observation);

typedef struct {
    BxFetchCrawlPlanOutputFn plan_output;
    BxFetchTransferHeadersCallback on_response_headers;
    BxFetchRunPrepareErrorFn on_prepare_error;
    BxFetchRunSubmitErrorFn on_submit_error;
    BxFetchRunCompletionFn on_completion;
    BxFetchRunRedirectFn allow_redirect;
    BxFetchRunDiscoveredLinkFn on_discovered_link;
    BxFetchRunDocumentErrorFn on_document_error;
    BxFetchRunLinkConversionFn on_link_conversion;
    BxFetchRunSeedResultFn on_seed_result;
    BxFetchTransportObserver transport_observer;
    BxFetchSchedulerObserver scheduler_observer;
    void* userdata;
} BxFetchRunFrontend;

typedef enum {
    BX_FETCH_RUN_FAILURE_NONE = 0,
    BX_FETCH_RUN_FAILURE_CONFIG,
    BX_FETCH_RUN_FAILURE_GLOBAL_INIT,
    BX_FETCH_RUN_FAILURE_CREATE,
    BX_FETCH_RUN_FAILURE_LOAD_PUBLICATION,
    BX_FETCH_RUN_FAILURE_LOAD_INPUT,
    BX_FETCH_RUN_FAILURE_ADD_SEED,
    BX_FETCH_RUN_FAILURE_EXECUTE,
    BX_FETCH_RUN_FAILURE_CONVERT_LINKS,
    BX_FETCH_RUN_FAILURE_SAVE_PUBLICATION,
} BxFetchRunFailureStage;

typedef struct {
    BxFetchRunFailureStage stage;
    int error_number;
    BxFetchRunSeedObservation seed;
    BxFetchInputOutcome input;
    BxFetchNetSetupError setup_error;
} BxFetchRunFailure;

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
/* Requires a successfully finished transfer run; config-gated no-op. */
int bx_fetch_run_convert_links(BxFetchRun* run);

/*
 * Executes the canonical one-shot applet lifecycle:
 *
 *   global init -> construct -> load -> seed -> execute -> convert -> save
 *
 * The run is always freed before process-global transport cleanup. Candidate
 * state is cancelled/drained by bx_fetch_run_free() on every unfinished path.
 * cfg, frontend, and callback userdata are borrowed for the duration of this
 * call. On failure, failure_out identifies the exact stage and retains setup
 * or seed detail without formatting frontend diagnostics.
 */
int bx_fetch_run_execute_config(const struct bx_fetch_config* cfg, const BxFetchRunFrontend* frontend, BxFetchRunFailure* failure_out);

#endif  // BX_FETCH_RUN_H
