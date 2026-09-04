#define _GNU_SOURCE
#include "lib/fetch/run.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/response.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct BxFetchRun {
    const struct bx_fetch_config* cfg;
    BxFetchRunFrontend frontend;
    BxFetchEngine* engine;
    BxFetchCrawlCoordinator* coordinator;
    BxFetchPublicationState* publication;
    struct RunDocumentTask* document_head;
    struct RunDocumentTask* document_tail;
    size_t document_count;
    size_t document_bytes;
    int deferred_error;
    bool publication_loaded;
};

typedef struct RunDocumentTask {
    BxFetchPreparedUrl* base;
    char* path;
    char* content_type;
    size_t accounted_bytes;
    int depth;
    struct RunDocumentTask* next;
} RunDocumentTask;

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

static void run_document_task_free(RunDocumentTask* task) {
    if (!task)
        return;
    bx_fetch_prepared_url_free(task->base);
    free(task->path);
    free(task->content_type);
    free(task);
}

static void run_clear_documents(BxFetchRun* run) {
    if (!run)
        return;
    RunDocumentTask* task = run->document_head;
    while (task) {
        RunDocumentTask* next = task->next;
        run_document_task_free(task);
        task = next;
    }
    run->document_head = NULL;
    run->document_tail = NULL;
    run->document_count = 0;
    run->document_bytes = 0;
}

static bool run_should_process_document(const BxFetchRun* run, const BxFetchTransferCompletion* completion, BxFetchPublicationResult publication) {
    return run && completion && publication == BX_FETCH_PUBLICATION_RECORDED && completion->result == BX_FETCH_OK && (run->cfg->recursive.recursive || run->cfg->recursive.page_requisites);
}

static int run_enqueue_document(BxFetchRun* run, const BxFetchTransferCompletion* completion, int depth) {
    const BxFetchPreparedUrl* base = bx_fetch_response_effective_target(completion->response);
    if (!base)
        base = bx_fetch_request_target(completion->request);
    if (!base || !completion->output_path) {
        errno = EINVAL;
        return -1;
    }

    const char* content_type = completion->response->content_type;
    size_t base_length = 0;
    size_t path_length = 0;
    size_t content_type_length = 0;
    if (!bx_fetch_resource_bounded_strlen(bx_fetch_prepared_url_transport(base), BX_FETCH_URL_MAX_BYTES, &base_length) ||
        !bx_fetch_resource_bounded_strlen(completion->output_path, BX_FETCH_URL_MAP_MAX_FIELD_BYTES, &path_length) ||
        (content_type && !bx_fetch_resource_bounded_strlen(content_type, BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES, &content_type_length)) || base_length > SIZE_MAX - path_length ||
        base_length + path_length > SIZE_MAX - content_type_length) {
        errno = EFBIG;
        return -1;
    }
    size_t accounted_bytes = base_length + path_length + content_type_length;
    if (!bx_fetch_resource_can_reserve(run->document_count, run->document_bytes, 1u, accounted_bytes, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        errno = EFBIG;
        return -1;
    }

    RunDocumentTask* task = calloc(1, sizeof(*task));
    if (!task)
        return -1;
    task->base = bx_fetch_prepared_url_clone(base);
    task->path = strdup(completion->output_path);
    task->content_type = content_type ? strdup(content_type) : NULL;
    task->accounted_bytes = accounted_bytes;
    task->depth = depth;
    if (!task->base || !task->path || (content_type && !task->content_type)) {
        run_document_task_free(task);
        return -1;
    }

    if (run->document_tail)
        run->document_tail->next = task;
    else
        run->document_head = task;
    run->document_tail = task;
    run->document_count++;
    run->document_bytes += accounted_bytes;
    return 0;
}

typedef struct {
    BxFetchRun* run;
    const RunDocumentTask* task;
} RunDocumentLinks;

static int run_add_document_link(void* userdata, const char* reference, BxFetchHtmlLinkKind kind) {
    RunDocumentLinks* links = userdata;
    BxFetchCrawlEnqueueResult result = bx_fetch_run_add_discovered(links->run, links->task->base, reference, kind, links->task->depth);
    if (links->run->frontend.on_discovered_link) {
        links->run->frontend.on_discovered_link(links->run->frontend.userdata, links->task->base, reference, kind, links->task->depth, result);
    }
    if (result.status == BX_FETCH_CRAWL_ERROR)
        return -1;
    return 0;
}

static int run_drain_documents(BxFetchRun* run) {
    while (run && run->document_head) {
        RunDocumentTask* task = run->document_head;
        run->document_head = task->next;
        if (!run->document_head)
            run->document_tail = NULL;
        run->document_count--;
        run->document_bytes -= task->accounted_bytes;
        task->next = NULL;

        RunDocumentLinks links = {
            .run = run,
            .task = task,
        };
        BxFetchDocumentOutcome outcome = {0};
        int result = bx_fetch_document_extract_links(task->path, task->content_type, task->base, run_add_document_link, &links, &outcome);
        int error_number = errno;
        bool keep_running = result == 0 || (run->frontend.on_document_error && run->frontend.on_document_error(run->frontend.userdata, task->base, task->path, task->depth, &outcome));
        run_document_task_free(task);
        if (!keep_running) {
            run_defer_failure(run, error_number);
            run_clear_documents(run);
            return -1;
        }
    }
    return 0;
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
    bool document_queued = false;
    if (publication == BX_FETCH_PUBLICATION_ERROR) {
        int error_number = errno ? errno : EIO;
        run_defer_failure(run, error_number);
        scheduler_result = error_number == EFBIG ? BX_FETCH_ERROR_RESOURCE_LIMIT : BX_FETCH_ERROR_INTERNAL;
        retryable_hint = false;
    }
    else if (run_should_process_document(run, completion, publication)) {
        if (run_enqueue_document(run, completion, transfer->depth) != 0) {
            int error_number = errno ? errno : EIO;
            run_defer_failure(run, error_number);
            scheduler_result = error_number == EFBIG ? BX_FETCH_ERROR_RESOURCE_LIMIT : BX_FETCH_ERROR_INTERNAL;
            retryable_hint = false;
        }
        else {
            document_queued = true;
        }
    }

    int status = completion->response ? completion->response->status_code : 0;
    bool retry_scheduled = transfer->scheduler_done(transfer->scheduler_done_userdata, status, scheduler_result, retryable_hint);

    BxFetchRunCompletion observation = {
        .transfer = completion,
        .depth = transfer->depth,
        .attempt = transfer->attempt,
        .max_attempts = transfer->max_attempts,
        .publication = publication,
        .document_queued = document_queued,
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
    if (!run->deferred_error)
        (void)run_drain_documents(run);
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
    run_clear_documents(run);
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
    run_clear_documents(run);
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

static int run_convert_download(void* userdata, const BxFetchDownloadedFileView* download) {
    BxFetchRun* run = userdata;
    BxFetchLinkConversionOutcome outcome = {0};
    int result = bx_fetch_document_convert_download(run->cfg, run->publication, download, &outcome);
    int error_number = errno;
    bool continue_after_failure = run->frontend.on_link_conversion && run->frontend.on_link_conversion(run->frontend.userdata, download, &outcome);
    if (result != 0 && !continue_after_failure) {
        errno = error_number ? error_number : EIO;
        return -1;
    }
    return 0;
}

int bx_fetch_run_convert_links(BxFetchRun* run) {
    if (!run || bx_fetch_run_phase(run) != BX_FETCH_CRAWL_PHASE_FINISHED) {
        errno = EINVAL;
        return -1;
    }
    if (!run->cfg->recursive.convert_links)
        return 0;
    return bx_fetch_publication_visit_downloads(run->publication, run_convert_download, run);
}

static void run_failure_reset(BxFetchRunFailure* failure) {
    if (!failure)
        return;
    *failure = (BxFetchRunFailure){
        .stage = BX_FETCH_RUN_FAILURE_NONE,
        .error_number = 0,
        .seed.index = -1,
    };
    failure->setup_error.curl_code = -1;
    failure->setup_error.error_number = -1;
}

static int run_session_fail(BxFetchRunFailure* failure,
                            BxFetchRunFailureStage stage,
                            int error_number,
                            const BxFetchRunSeedObservation* seed,
                            const BxFetchInputOutcome* input,
                            const BxFetchNetSetupError* setup_error) {
    if (error_number <= 0)
        error_number = EIO;
    if (failure) {
        failure->stage = stage;
        failure->error_number = error_number;
        if (seed)
            failure->seed = *seed;
        if (input)
            failure->input = *input;
        if (setup_error)
            failure->setup_error = *setup_error;
    }
    errno = error_number;
    return -1;
}

static int run_session_add_seed(BxFetchRun* run, int index, const char* url, const char* source_path, BxFetchRunSeedObservation* failed_seed) {
    errno = 0;
    BxFetchPreparedUrl* target = NULL;
    BxFetchCrawlEnqueueResult enqueue = bx_fetch_crawl_coordinator_add_seed_observed(run->coordinator, url, &target);
    BxFetchRunSeedObservation observation = {
        .index = index,
        .result = enqueue,
        .error_number = errno,
        .source_path = source_path,
        .target = target,
    };
    bool accepted = enqueue.status == BX_FETCH_CRAWL_ENQUEUED || enqueue.status == BX_FETCH_CRAWL_SKIPPED_DUPLICATE;
    if (!accepted && observation.error_number <= 0)
        observation.error_number = enqueue.status == BX_FETCH_CRAWL_REJECTED ? EPERM : EIO;
    bool frontend_continue = run->frontend.on_seed_result && run->frontend.on_seed_result(run->frontend.userdata, &observation);
    if (accepted || (enqueue.status == BX_FETCH_CRAWL_REJECTED && frontend_continue)) {
        bx_fetch_prepared_url_free(target);
        return 0;
    }
    *failed_seed = observation;
    /*
     * failure_out cannot retain a borrowed target. Typed status, index,
     * source path, and errno remain sufficient after the observer rendered it.
     */
    failed_seed->target = NULL;
    bx_fetch_prepared_url_free(target);
    errno = observation.error_number;
    return -1;
}

int bx_fetch_run_execute_config(const struct bx_fetch_config* cfg, const BxFetchRunFrontend* frontend, BxFetchRunFailure* failure_out) {
    run_failure_reset(failure_out);
    bool has_input_file = cfg && cfg->input.input_file && cfg->input.input_file[0] != '\0';
    if (!cfg || !frontend || !frontend->plan_output || cfg->input.url_count < 0 || (cfg->input.url_count == 0 && !has_input_file) ||
        (cfg->input.url_count > 0 && !cfg->input.urls) || (cfg->input.force_html && !has_input_file) ||
        (cfg->input.base_url && (!has_input_file || !cfg->input.force_html))) {
        return run_session_fail(failure_out, BX_FETCH_RUN_FAILURE_CONFIG, EINVAL, NULL, NULL, NULL);
    }
    size_t direct_url_bytes = 0;
    for (int index = 0; index < cfg->input.url_count; index++) {
        size_t length = 0;
        if (!cfg->input.urls[index] || !bx_fetch_resource_bounded_strlen(cfg->input.urls[index], BX_FETCH_URL_MAX_BYTES, &length) ||
            !bx_fetch_resource_can_reserve((size_t)index, direct_url_bytes, 1u, length, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
            BxFetchRunSeedObservation seed = {
                .index = index,
                .error_number = EINVAL,
            };
            return run_session_fail(failure_out, BX_FETCH_RUN_FAILURE_CONFIG, EINVAL, &seed, NULL, NULL);
        }
        direct_url_bytes += length;
    }

    BxFetchInputUrls input_urls = {0};
    if (has_input_file) {
        BxFetchInputOutcome input_outcome;
        int input_result = cfg->input.force_html
                               ? bx_fetch_input_urls_load_html(cfg->input.input_file,
                                                               cfg->input.base_url,
                                                               (size_t)cfg->input.url_count,
                                                               direct_url_bytes,
                                                               &input_urls,
                                                               &input_outcome)
                               : bx_fetch_input_urls_load_plain(
                                     cfg->input.input_file, (size_t)cfg->input.url_count, direct_url_bytes, &input_urls, &input_outcome);
        if (input_result != 0) {
            return run_session_fail(failure_out, BX_FETCH_RUN_FAILURE_LOAD_INPUT, input_outcome.error_number, NULL, &input_outcome, NULL);
        }
    }

    bool global_initialized = false;
    BxFetchRun* run = NULL;
    BxFetchRunFailureStage failure_stage = BX_FETCH_RUN_FAILURE_NONE;
    int error_number = 0;
    BxFetchRunSeedObservation failed_seed = {
        .index = -1,
    };
    BxFetchNetSetupError setup_error = {
        .curl_code = -1,
        .error_number = -1,
    };

    if (!cfg->download.dry_run) {
        struct bx_fetch_config capability_config = *cfg;
        char** capability_urls = NULL;
        if (input_urls.count > 0) {
            /*
             * Capability policy must see file-derived protocols before any
             * transfer starts. This cold, shallow snapshot borrows every
             * config field and replaces only the temporary URL pointer view.
             */
            size_t total_urls = (size_t)cfg->input.url_count + input_urls.count;
            capability_urls = malloc(total_urls * sizeof(*capability_urls));
            if (!capability_urls) {
                BxFetchInputOutcome input_outcome = {
                    .kind = BX_FETCH_INPUT_FAILURE_MEMORY,
                    .error_number = ENOMEM,
                };
                bx_fetch_input_urls_free(&input_urls);
                return run_session_fail(failure_out, BX_FETCH_RUN_FAILURE_LOAD_INPUT, ENOMEM, NULL, &input_outcome, NULL);
            }
            for (int index = 0; index < cfg->input.url_count; index++)
                capability_urls[index] = cfg->input.urls[index];
            for (size_t index = 0; index < input_urls.count; index++)
                capability_urls[(size_t)cfg->input.url_count + index] = input_urls.urls[index];
            capability_config.input.urls = capability_urls;
            capability_config.input.url_count = (int)total_urls;
        }
        errno = 0;
        if (bx_fetch_global_init(&capability_config, &setup_error) != 0) {
            error_number = setup_error.error_number > 0 ? setup_error.error_number : (errno > 0 ? errno : EIO);
            free(capability_urls);
            bx_fetch_input_urls_free(&input_urls);
            return run_session_fail(failure_out, BX_FETCH_RUN_FAILURE_GLOBAL_INIT, error_number, NULL, NULL, &setup_error);
        }
        free(capability_urls);
        global_initialized = true;
    }

    run = bx_fetch_run_new(cfg, frontend);
    if (!run) {
        failure_stage = BX_FETCH_RUN_FAILURE_CREATE;
        error_number = errno;
        goto cleanup;
    }
    if (bx_fetch_run_load_publication(run) != 0) {
        failure_stage = BX_FETCH_RUN_FAILURE_LOAD_PUBLICATION;
        error_number = errno;
        goto cleanup;
    }

    for (int index = 0; index < cfg->input.url_count; index++) {
        if (run_session_add_seed(run, index, cfg->input.urls[index], NULL, &failed_seed) != 0)
            goto seed_failure;
    }
    for (size_t input_index = 0; input_index < input_urls.count; input_index++) {
        int index = cfg->input.url_count + (int)input_index;
        if (run_session_add_seed(run, index, input_urls.urls[input_index], cfg->input.input_file, &failed_seed) != 0)
            goto seed_failure;
    }
    if (bx_fetch_run_execute(run) != 0) {
        failure_stage = BX_FETCH_RUN_FAILURE_EXECUTE;
        error_number = errno;
        goto cleanup;
    }
    if (bx_fetch_run_convert_links(run) != 0) {
        failure_stage = BX_FETCH_RUN_FAILURE_CONVERT_LINKS;
        error_number = errno;
        goto cleanup;
    }
    if (bx_fetch_run_save_publication(run) != 0) {
        failure_stage = BX_FETCH_RUN_FAILURE_SAVE_PUBLICATION;
        error_number = errno;
        goto cleanup;
    }

cleanup:
    bx_fetch_run_free(run);
    bx_fetch_input_urls_free(&input_urls);
    if (global_initialized)
        bx_fetch_global_cleanup();
    if (failure_stage != BX_FETCH_RUN_FAILURE_NONE)
        return run_session_fail(failure_out, failure_stage, error_number, failed_seed.index >= 0 ? &failed_seed : NULL, NULL, NULL);
    return 0;

seed_failure:
    failure_stage = BX_FETCH_RUN_FAILURE_ADD_SEED;
    error_number = failed_seed.error_number;
    goto cleanup;
}
