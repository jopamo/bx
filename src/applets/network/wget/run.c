#define _GNU_SOURCE
#include "wget.h"
#include "lib/fetch/error.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/pathmap.h"
#include "lib/fetch/run.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const struct bx_fetch_config* config;
    int exit_code;
} WgetRunFrontend;

static bool wget_is_quiet(const WgetRunFrontend* frontend) {
    return frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET;
}

static void wget_record_error(WgetRunFrontend* frontend, int exit_code, const char* summary) {
    if (!frontend)
        return;
    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, exit_code);
    if (!wget_is_quiet(frontend))
        fprintf(stderr, "wget: %s\n", summary);
}

static int wget_plan_output(void* userdata, const BxFetchPreparedUrl* target, int depth, char** output_path_out) {
    WgetRunFrontend* frontend = userdata;
    if (!frontend || !target || !output_path_out || depth != 0) {
        errno = EINVAL;
        return -1;
    }

    if (frontend->config->download.output_document)
        *output_path_out = strdup(frontend->config->download.output_document);
    else
        *output_path_out = bx_fetch_pathmap_canonical_url_to_local(bx_fetch_prepared_url_transport(target), frontend->config);
    return *output_path_out ? 0 : -1;
}

static void wget_prepare_error(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchPrepareError* error) {
    (void)target;
    (void)output_path;
    WgetRunFrontend* frontend = userdata;
    int exit_code = error && error->kind == BX_FETCH_PREPARE_FAILURE_PROTOCOL_POLICY ? BX_FETCH_EXIT_PROTOCOL : BX_FETCH_EXIT_FILE_IO;
    wget_record_error(frontend, exit_code, "failed to prepare output");
}

static void wget_submit_error(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchNetSetupError* error) {
    (void)target;
    (void)output_path;
    (void)error;
    wget_record_error(userdata, BX_FETCH_EXIT_NETWORK, "failed to start transfer");
}

static int wget_completion(void* userdata, BxFetchRun* run, const BxFetchRunCompletion* completion) {
    (void)run;
    WgetRunFrontend* frontend = userdata;
    if (!frontend || !completion || !completion->transfer)
        return -1;

    const BxFetchTransferCompletion* transfer = completion->transfer;
    if (completion->retry_scheduled)
        return 0;
    if (transfer->result != BX_FETCH_OK) {
        const BxFetchResponse* response = transfer->response;
        int status = response ? response->status_code : 0;
        BxFetchTransportErrorKind transport_kind = response ? response->transport_error_kind : BX_FETCH_TRANSPORT_ERROR_NONE;
        int exit_code = bx_fetch_exit_code_for_transfer_failure(status, transport_kind, transfer->result);
        if (!wget_is_quiet(frontend)) {
            if (status >= 400 && status < 600)
                fprintf(stderr, "wget: server returned error: HTTP %d\n", status);
            else
                fprintf(stderr, "wget: transfer failed: %s\n", bx_fetch_error_string(transfer->result));
        }
        frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, exit_code);
        return 0;
    }

    if (frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_VERBOSE && !frontend->config->download.spider) {
        fprintf(stderr, "'%s' saved\n", transfer->output_path);
    }
    return 0;
}

static bool wget_seed_result(void* userdata, const BxFetchRunSeedObservation* observation) {
    WgetRunFrontend* frontend = userdata;
    if (!frontend || !observation)
        return false;
    if (observation->result.status == BX_FETCH_CRAWL_REJECTED) {
        wget_record_error(frontend,
                          BX_FETCH_EXIT_PROTOCOL,
                          observation->result.filter_decision == FILTER_DECISION_INVALID_URL ? "invalid URL" : "URL rejected by protocol policy");
        return true;
    }
    if (observation->result.status == BX_FETCH_CRAWL_ERROR)
        wget_record_error(frontend, BX_FETCH_EXIT_PROTOCOL, "invalid URL");
    return false;
}

static void wget_response_header(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, const char* raw_header, size_t raw_header_len) {
    (void)request;
    (void)response;
    WgetRunFrontend* frontend = userdata;
    if (!frontend || !frontend->config->download.server_response || wget_is_quiet(frontend))
        return;
    (void)fwrite(raw_header, 1, raw_header_len, stderr);
}

static void wget_retry(void* userdata, const BxFetchPreparedUrl* target, int next_attempt, int max_attempts, int delay_seconds) {
    (void)target;
    WgetRunFrontend* frontend = userdata;
    if (!frontend || wget_is_quiet(frontend))
        return;
    fprintf(stderr, "wget: retrying (%d/%d) in %d second%s\n", next_attempt, max_attempts, delay_seconds, delay_seconds == 1 ? "" : "s");
}

static void wget_record_session_failure(WgetRunFrontend* frontend, const BxFetchRunFailure* failure) {
    if (!frontend || !failure)
        return;

    switch (failure->stage) {
        case BX_FETCH_RUN_FAILURE_CONFIG:
            wget_record_error(frontend, BX_FETCH_EXIT_PARSE_OR_CONFIG, "invalid fetch configuration");
            break;
        case BX_FETCH_RUN_FAILURE_GLOBAL_INIT:
        case BX_FETCH_RUN_FAILURE_CREATE:
            wget_record_error(frontend, BX_FETCH_EXIT_NETWORK, "failed to initialize network transport");
            break;
        case BX_FETCH_RUN_FAILURE_LOAD_PUBLICATION:
        case BX_FETCH_RUN_FAILURE_SAVE_PUBLICATION:
            wget_record_error(frontend, BX_FETCH_EXIT_FILE_IO, "failed to access persistent fetch state");
            break;
        case BX_FETCH_RUN_FAILURE_LOAD_INPUT:
            wget_record_error(frontend, BX_FETCH_EXIT_FILE_IO, "failed to read URL input");
            break;
        case BX_FETCH_RUN_FAILURE_ADD_SEED:
            wget_record_error(frontend, BX_FETCH_EXIT_PROTOCOL, "invalid or unsupported URL");
            break;
        case BX_FETCH_RUN_FAILURE_EXECUTE:
            wget_record_error(frontend, BX_FETCH_EXIT_NETWORK, "fetch run failed");
            break;
        case BX_FETCH_RUN_FAILURE_CONVERT_LINKS:
            wget_record_error(frontend, BX_FETCH_EXIT_FILE_IO, "link conversion failed");
            break;
        case BX_FETCH_RUN_FAILURE_NONE:
            break;
    }
}

int bx_wget_run_config(const struct bx_fetch_config* config) {
    if (!config || config->input.url_count <= 0 || !config->input.urls) {
        errno = EINVAL;
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    WgetRunFrontend frontend_state = {
        .config = config,
    };
    BxFetchRunFrontend frontend = {
        .plan_output = wget_plan_output,
        .on_prepare_error = wget_prepare_error,
        .on_submit_error = wget_submit_error,
        .on_completion = wget_completion,
        .on_seed_result = wget_seed_result,
        .transport_observer =
            {
                .on_response_header = wget_response_header,
                .userdata = &frontend_state,
            },
        .scheduler_observer =
            {
                .on_retry = wget_retry,
                .userdata = &frontend_state,
            },
        .userdata = &frontend_state,
    };

    BxFetchRunFailure failure;
    if (bx_fetch_run_execute_config(config, &frontend, &failure) != 0) {
        bool callback_already_reported = frontend_state.exit_code != BX_FETCH_EXIT_SUCCESS && (failure.stage == BX_FETCH_RUN_FAILURE_ADD_SEED || failure.stage == BX_FETCH_RUN_FAILURE_EXECUTE);
        if (!callback_already_reported)
            wget_record_session_failure(&frontend_state, &failure);
    }
    return frontend_state.exit_code;
}
