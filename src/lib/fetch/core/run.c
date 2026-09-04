#define _GNU_SOURCE
#include "lib/fetch/run.h"
#include <errno.h>
#include <stdlib.h>

struct BxFetchRun {
    const struct bx_fetch_config* cfg;
    BxFetchRunFrontend frontend;
    BxFetchEngine* engine;
    BxFetchCrawlCoordinator* coordinator;
    BxFetchPublicationState* publication;
    int deferred_error;
    bool publication_loaded;
};

typedef struct {
    BxFetchRun* run;
    BxFetchSchedulerTransferDoneFn scheduler_done;
    void* scheduler_done_userdata;
    int depth;
    int attempt;
    int max_attempts;
} RunTransfer;

static void run_defer_failure(BxFetchRun* run, int error_number) {
    if (run && !run->deferred_error)
        run->deferred_error = error_number > 0 ? error_number : EIO;
}

static int run_plan_output(void* userdata, const BxFetchPreparedUrl* target, int depth, char** output_path_out) {
    BxFetchRun* run = userdata;
    return run->frontend.plan_output(run->frontend.userdata, target, depth, output_path_out);
}

static int run_response_headers(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer) {
    RunTransfer* transfer = userdata;
    BxFetchRun* run = transfer ? transfer->run : NULL;
    if (!run) {
        errno = EINVAL;
        return -1;
    }
    if (!run->frontend.on_response_headers)
        return 0;
    return run->frontend.on_response_headers(run->frontend.userdata, request, response, writer);
}

static void run_transfer_complete(void* userdata, const BxFetchTransferCompletion* completion) {
    RunTransfer* transfer = userdata;
    BxFetchRun* run = transfer ? transfer->run : NULL;
    if (!run || !completion) {
        if (run)
            run_defer_failure(run, EPROTO);
        if (transfer && transfer->scheduler_done)
            transfer->scheduler_done(transfer->scheduler_done_userdata, 0, BX_FETCH_ERROR_INTERNAL, false);
        free(transfer);
        return;
    }

    BxFetchPublicationResult publication = bx_fetch_publication_record_completion(run->publication, completion);
    BxFetchError scheduler_result = completion->result;
    bool retryable_hint = completion->retryable_hint;
    if (publication == BX_FETCH_PUBLICATION_ERROR) {
        int error_number = errno ? errno : EIO;
        run_defer_failure(run, error_number);
        scheduler_result = error_number == EFBIG ? BX_FETCH_ERROR_RESOURCE_LIMIT : BX_FETCH_ERROR_INTERNAL;
        retryable_hint = false;
    }

    int status = completion->response ? completion->response->status_code : 0;
    bool retry_scheduled = transfer->scheduler_done(transfer->scheduler_done_userdata, status, scheduler_result, retryable_hint);

    BxFetchRunCompletion observation = {
        .transfer = completion,
        .depth = transfer->depth,
        .attempt = transfer->attempt,
        .max_attempts = transfer->max_attempts,
        .publication = publication,
        .retry_scheduled = retry_scheduled,
    };
    if (run->frontend.on_completion && run->frontend.on_completion(run->frontend.userdata, run, &observation) != 0) {
        run_defer_failure(run, errno);
    }

    free(transfer);
}

static bool run_redirect_allowed(void* userdata, const BxFetchPreparedUrl* target) {
    RunTransfer* transfer = userdata;
    BxFetchRun* run = transfer ? transfer->run : NULL;
    BxFetchFilterDecision decision = FILTER_DECISION_INVALID_URL;
    if (!run || bx_fetch_crawl_coordinator_evaluate_target(run->coordinator, target, &decision) != 0) {
        return false;
    }

    bool frontend_allowed = !run->frontend.allow_redirect || run->frontend.allow_redirect(run->frontend.userdata, target, decision);
    return decision == FILTER_DECISION_ACCEPT && frontend_allowed;
}

static int
run_dispatch(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, int depth, int attempt, int max_attempts, BxFetchSchedulerTransferDoneFn on_done, void* done_userdata) {
    BxFetchRun* run = userdata;
    if (!run || !target || !output_path || !on_done) {
        errno = EINVAL;
        return -1;
    }
    if (run->deferred_error) {
        errno = run->deferred_error;
        return -1;
    }
    if (run->cfg->download.dry_run)
        return 1;

    BxFetchPrepareError prepare_error = {0};
    BxFetchTransferCandidate* candidate = bx_fetch_transfer_candidate_prepare(run->cfg, target, output_path, &prepare_error);
    if (!candidate) {
        if (run->frontend.on_prepare_error) {
            run->frontend.on_prepare_error(run->frontend.userdata, target, output_path, &prepare_error);
        }
        return -1;
    }

    RunTransfer* transfer = calloc(1, sizeof(*transfer));
    if (!transfer) {
        bx_fetch_transfer_candidate_abort(candidate);
        return -1;
    }
    transfer->run = run;
    transfer->scheduler_done = on_done;
    transfer->scheduler_done_userdata = done_userdata;
    transfer->depth = depth;
    transfer->attempt = attempt;
    transfer->max_attempts = max_attempts;

    BxFetchNetSetupError setup_error = {0};
    if (bx_fetch_transfer_candidate_submit(candidate, run->engine, run_response_headers, run_transfer_complete, transfer, run_redirect_allowed, transfer, &setup_error) != 0) {
        if (run->frontend.on_submit_error) {
            run->frontend.on_submit_error(run->frontend.userdata, target, output_path, &setup_error);
        }
        free(transfer);
        return -1;
    }
    return 0;
}

static int run_transport_poll(void* userdata) {
    BxFetchRun* run = userdata;
    if (!run || !run->engine) {
        errno = EINVAL;
        return -1;
    }

    if (bx_fetch_engine_run(run->engine) != 0)
        run_defer_failure(run, errno);
    if (run->deferred_error) {
        bx_fetch_crawl_coordinator_cancel(run->coordinator);
        bx_fetch_engine_cancel(run->engine);
    }
    /*
     * Cancellation invokes every active completion synchronously. Return
     * success so the scheduler can observe those callbacks and drain its
     * active accounting before the facade reports the deferred failure.
     */
    return 0;
}

BxFetchRun* bx_fetch_run_new(const struct bx_fetch_config* cfg, const BxFetchRunFrontend* frontend) {
    if (!cfg || !frontend || !frontend->plan_output) {
        errno = EINVAL;
        return NULL;
    }

    BxFetchRun* run = calloc(1, sizeof(*run));
    if (!run)
        return NULL;
    run->cfg = cfg;
    run->frontend = *frontend;

    run->publication = bx_fetch_publication_state_new(cfg);
    if (!run->publication)
        goto fail;
    if (!cfg->download.dry_run) {
        run->engine = bx_fetch_engine_new(cfg, &frontend->transport_observer);
        if (!run->engine)
            goto fail;
    }
    run->coordinator = bx_fetch_crawl_coordinator_new(cfg, run_plan_output, run_dispatch, cfg->download.dry_run ? NULL : run_transport_poll, run, &frontend->scheduler_observer);
    if (!run->coordinator)
        goto fail;
    return run;

fail:
    bx_fetch_run_free(run);
    return NULL;
}

void bx_fetch_run_free(BxFetchRun* run) {
    if (!run)
        return;
    if (run->coordinator)
        bx_fetch_crawl_coordinator_cancel(run->coordinator);
    if (run->engine)
        bx_fetch_engine_cancel(run->engine);
    bx_fetch_crawl_coordinator_free(run->coordinator);
    bx_fetch_engine_free(run->engine);
    bx_fetch_publication_state_free(run->publication);
    free(run);
}

BxFetchCrawlEnqueueResult bx_fetch_run_add_seed(BxFetchRun* run, const char* url) {
    return bx_fetch_crawl_coordinator_add_seed(run ? run->coordinator : NULL, url);
}

BxFetchCrawlEnqueueResult bx_fetch_run_add_discovered(BxFetchRun* run, const BxFetchPreparedUrl* base, const char* reference, BxFetchHtmlLinkKind kind, int parent_depth) {
    return bx_fetch_crawl_coordinator_add_discovered(run ? run->coordinator : NULL, base, reference, kind, parent_depth);
}

int bx_fetch_run_execute(BxFetchRun* run) {
    if (!run || !run->coordinator || run->deferred_error || bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_COLLECTING) {
        BxFetchCrawlPhase phase = bx_fetch_run_phase(run);
        errno = run && run->deferred_error ? run->deferred_error : EINVAL;
        if (run && phase == BX_FETCH_CRAWL_PHASE_CANCELLED)
            errno = ECANCELED;
        return -1;
    }

    int result = bx_fetch_crawl_coordinator_run(run->coordinator);
    if (result != 0 || run->deferred_error) {
        int error_number = run->deferred_error ? run->deferred_error : (errno ? errno : EIO);
        bx_fetch_run_cancel(run);
        errno = error_number;
        return -1;
    }
    return 0;
}

void bx_fetch_run_cancel(BxFetchRun* run) {
    if (!run)
        return;
    bx_fetch_crawl_coordinator_cancel(run->coordinator);
    bx_fetch_engine_cancel(run->engine);
}

BxFetchCrawlPhase bx_fetch_run_phase(const BxFetchRun* run) {
    return run ? bx_fetch_crawl_coordinator_phase(run->coordinator) : BX_FETCH_CRAWL_PHASE_FAILED;
}

int bx_fetch_run_load_publication(BxFetchRun* run) {
    if (!run || run->publication_loaded || bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_COLLECTING) {
        errno = EINVAL;
        return -1;
    }
    if (bx_fetch_publication_load_persisted_mappings(run->publication) != 0)
        return -1;
    run->publication_loaded = true;
    return 0;
}

int bx_fetch_run_save_publication(const BxFetchRun* run) {
    if (!run || (bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_FINISHED && bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_FAILED && bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_CANCELLED)) {
        errno = EINVAL;
        return -1;
    }
    return bx_fetch_publication_save_persisted_mappings(run->publication);
}

const BxFetchPublicationState* bx_fetch_run_publication(const BxFetchRun* run) {
    return run ? run->publication : NULL;
}
