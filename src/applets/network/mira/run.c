#define _GNU_SOURCE
#include "logging.h"
#include "mira.h"
#include "lib/fetch/error.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/filter.h"
#include "lib/fetch/pathmap.h"
#include "lib/fetch/run.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BxFetchCrawlEnqueueStatus status;
    bool observed;
    char* display_url;
    char* normalized_url;
    char* reason;
    char* output_path;
} MiraDryRunRecord;

typedef struct {
    const struct bx_fetch_config* config;
    int exit_code;
    int last_progress_percent;
    bool progress_line_active;
    MiraDryRunRecord* dry_run_records;
    size_t dry_run_record_count;
    size_t next_plan_index;
    int deferred_error;
    FILE* diagnostics;
} MiraRunFrontend;

static void mira_json_string(FILE* stream, const char* value) {
    fputc('"', stream);
    for (const unsigned char* cursor = (const unsigned char*)value; *cursor; cursor++) {
        switch (*cursor) {
            case '"':
                fputs("\\\"", stream);
                break;
            case '\\':
                fputs("\\\\", stream);
                break;
            case '\b':
                fputs("\\b", stream);
                break;
            case '\f':
                fputs("\\f", stream);
                break;
            case '\n':
                fputs("\\n", stream);
                break;
            case '\r':
                fputs("\\r", stream);
                break;
            case '\t':
                fputs("\\t", stream);
                break;
            default:
                if (*cursor < 0x20)
                    fprintf(stream, "\\u%04x", *cursor);
                else
                    fputc(*cursor, stream);
                break;
        }
    }
    fputc('"', stream);
}

static void mira_run_record_error_url(MiraRunFrontend* frontend, BxFetchErrorClass error_class, const char* summary, const char* display_url, const char* path, int error_number) {
    if (!frontend)
        return;
    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, bx_fetch_exit_code_for_error_class(error_class, -1));
    if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
        if (display_url)
            fprintf(frontend->diagnostics, "mira: %s: %s\n", summary, display_url);
        else
            fprintf(frontend->diagnostics, "mira: %s\n", summary);
    }
    if (frontend->config->logging.structured_errors) {
        bx_fetch_error_emit_simple(frontend->diagnostics, error_class, summary, display_url, path, -1, error_number);
    }
}

static void mira_run_record_error(MiraRunFrontend* frontend, BxFetchErrorClass error_class, const char* summary, const BxFetchPreparedUrl* target, const char* path, int error_number) {
    mira_run_record_error_url(frontend, error_class, summary, target ? bx_fetch_prepared_url_display(target) : NULL, path, error_number);
}

static int mira_plan_output(void* userdata, const BxFetchPreparedUrl* target, int depth, char** output_path_out) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !target || !output_path_out || depth < 0) {
        errno = EINVAL;
        return -1;
    }
    if (frontend->deferred_error) {
        errno = frontend->deferred_error;
        return -1;
    }

    if (frontend->config->download.output_document)
        *output_path_out = strdup(frontend->config->download.output_document);
    else
        *output_path_out = bx_fetch_pathmap_canonical_url_to_local(bx_fetch_prepared_url_transport(target), frontend->config);
    if (!*output_path_out)
        return -1;

    if (frontend->config->download.dry_run) {
        while (frontend->next_plan_index < frontend->dry_run_record_count && frontend->dry_run_records[frontend->next_plan_index].status != BX_FETCH_CRAWL_ENQUEUED) {
            frontend->next_plan_index++;
        }
        if (frontend->next_plan_index >= frontend->dry_run_record_count) {
            free(*output_path_out);
            *output_path_out = NULL;
            errno = EPROTO;
            return -1;
        }
        MiraDryRunRecord* record = &frontend->dry_run_records[frontend->next_plan_index++];
        record->display_url = strdup(bx_fetch_prepared_url_display(target));
        record->normalized_url = strdup(bx_fetch_prepared_url_display(target));
        record->output_path = strdup(*output_path_out);
        if (!record->display_url || !record->normalized_url || !record->output_path) {
            frontend->deferred_error = ENOMEM;
            errno = ENOMEM;
            return -1;
        }
    }
    return 0;
}

static void mira_prepare_error(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchPrepareError* error) {
    MiraRunFrontend* frontend = userdata;
    BxFetchErrorClass error_class = error && error->kind == BX_FETCH_PREPARE_FAILURE_PROTOCOL_POLICY ? BX_FETCH_ERROR_CLASS_POLICY : BX_FETCH_ERROR_CLASS_FILESYSTEM;
    mira_run_record_error(frontend, error_class, "failed to prepare transfer", target, output_path, error ? error->error_number : EIO);
}

static void mira_submit_error(void* userdata, const BxFetchPreparedUrl* target, const char* output_path, const BxFetchNetSetupError* error) {
    mira_run_record_error(userdata, BX_FETCH_ERROR_CLASS_CURL_TRANSPORT, "failed to submit transfer", target, output_path, error ? error->error_number : EIO);
}

static int mira_completion(void* userdata, BxFetchRun* run, const BxFetchRunCompletion* completion) {
    (void)run;
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !completion || !completion->transfer)
        return -1;
    if (completion->retry_scheduled)
        return 0;
    if (frontend->progress_line_active) {
        fputc('\n', frontend->diagnostics);
        frontend->progress_line_active = false;
    }
    frontend->last_progress_percent = -1;
    if (completion->transfer->result == BX_FETCH_OK) {
        if (frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_VERBOSE && !frontend->config->download.spider) {
            fprintf(frontend->diagnostics, "mira: saved %s\n", completion->transfer->output_path);
        }
        return 0;
    }

    const BxFetchResponse* response = completion->transfer->response;
    int status = response ? response->status_code : 0;
    BxFetchTransportErrorKind transport_kind = response ? response->transport_error_kind : BX_FETCH_TRANSPORT_ERROR_NONE;
    int exit_code = bx_fetch_exit_code_for_transfer_failure(status, transport_kind, completion->transfer->result);
    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, exit_code);
    if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
        fprintf(frontend->diagnostics, "mira: transfer failed: %s\n", bx_fetch_error_string(completion->transfer->result));
    }
    if (frontend->config->logging.structured_errors) {
        const BxFetchRequest* request = completion->transfer->request;
        BxFetchStructuredError error = {
            .class_id = bx_fetch_error_class_for_exit_code(exit_code),
            .summary = "transfer failed",
            .url = request ? bx_fetch_request_url_for_display(request) : NULL,
            .path = completion->transfer->output_path,
            .http_status = status > 0 ? status : -1,
            .curl_code = response && response->error_code != 0 ? response->error_code : -1,
            .error_number = response && response->error_number > 0 ? response->error_number : -1,
            .retryable = completion->transfer->retryable_hint,
            .attempt = completion->attempt,
            .max_attempts = completion->max_attempts,
        };
        bx_fetch_error_emit_structured(frontend->diagnostics, &error);
    }
    return 0;
}

static bool mira_seed_result(void* userdata, const BxFetchRunSeedObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !observation)
        return false;
    if (frontend->config->download.dry_run && observation->index >= 0 && (size_t)observation->index < frontend->dry_run_record_count) {
        MiraDryRunRecord* record = &frontend->dry_run_records[observation->index];
        record->observed = true;
        record->status = observation->result.status;
        if (observation->result.status == BX_FETCH_CRAWL_REJECTED && observation->target) {
            record->display_url = strdup(bx_fetch_prepared_url_display(observation->target));
            record->normalized_url = strdup(bx_fetch_prepared_url_display(observation->target));
            const char* reason = bx_fetch_filter_decision_reason(observation->result.filter_decision);
            record->reason = strdup(reason ? reason : "unspecified");
            if (!record->display_url || !record->normalized_url || !record->reason) {
                frontend->deferred_error = ENOMEM;
                mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to allocate dry-run plan", NULL, NULL, ENOMEM);
                return false;
            }
        }
        else if (observation->result.status == BX_FETCH_CRAWL_ERROR && observation->error_number != ENOMEM) {
            record->display_url = strdup("[URL redacted]");
            record->reason = strdup(observation->error_number == EPROTONOSUPPORT ? "unsupported-protocol" : "invalid-url");
            if (!record->display_url || !record->reason) {
                frontend->deferred_error = ENOMEM;
                mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to allocate dry-run plan", NULL, NULL, ENOMEM);
                return false;
            }
        }
    }

    if (observation->result.status == BX_FETCH_CRAWL_ERROR) {
        if (observation->error_number == ENOMEM) {
            frontend->deferred_error = ENOMEM;
            return false;
        }
        const char* summary = observation->error_number == EPROTONOSUPPORT ? "initial URL uses an unsupported protocol" : "invalid initial URL";
        BxFetchErrorClass error_class = observation->error_number == EPROTONOSUPPORT ? BX_FETCH_ERROR_CLASS_CURL_TRANSPORT : BX_FETCH_ERROR_CLASS_POLICY;
        mira_run_record_error_url(frontend, error_class, summary, "[URL redacted]", NULL, -1);
        return false;
    }
    if (observation->result.status != BX_FETCH_CRAWL_REJECTED)
        return false;
    const char* reason = bx_fetch_filter_decision_reason(observation->result.filter_decision);
    if (observation->result.filter_decision == FILTER_DECISION_URL_CREDENTIALS) {
        mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_POLICY, "URL credentials are disabled by --paranoid", observation->target, NULL, -1);
        return false;
    }
    if (observation->result.filter_decision == FILTER_DECISION_HTTPS_ONLY) {
        if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
            fprintf(frontend->diagnostics, "mira: skipping non-HTTPS URL due to --https-only: %s\n", bx_fetch_prepared_url_display(observation->target));
        }
        return true;
    }
    if (!frontend->config->download.dry_run && frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
        fprintf(frontend->diagnostics, "mira: URL rejected by fetch policy: %s\n", reason ? reason : "unspecified");
    }
    return true;
}

static void mira_dry_run_records_free(MiraRunFrontend* frontend) {
    if (!frontend)
        return;
    for (size_t index = 0; index < frontend->dry_run_record_count; index++) {
        MiraDryRunRecord* record = &frontend->dry_run_records[index];
        free(record->display_url);
        free(record->normalized_url);
        free(record->reason);
        free(record->output_path);
    }
    free(frontend->dry_run_records);
    frontend->dry_run_records = NULL;
    frontend->dry_run_record_count = 0;
}

static void mira_dry_run_records_emit(const MiraRunFrontend* frontend) {
    unsigned long sequence = 0;
    for (size_t index = 0; index < frontend->dry_run_record_count; index++) {
        const MiraDryRunRecord* record = &frontend->dry_run_records[index];
        if (!record->observed || (record->status != BX_FETCH_CRAWL_ENQUEUED && record->status != BX_FETCH_CRAWL_ERROR && record->status != BX_FETCH_CRAWL_REJECTED)) {
            continue;
        }
        fprintf(stdout,
                "{\"schema_version\":1,\"event\":\"dry-run-plan\","
                "\"sequence\":%lu,\"url\":",
                sequence++);
        mira_json_string(stdout, record->display_url);
        fputs(",\"normalized_url\":", stdout);
        if (record->normalized_url)
            mira_json_string(stdout, record->normalized_url);
        else
            fputs("null", stdout);
        if (record->status == BX_FETCH_CRAWL_ENQUEUED) {
            fputs(",\"decision\":\"fetch\",\"reason\":null,\"output_path\":", stdout);
            mira_json_string(stdout, record->output_path);
        }
        else {
            fputs(",\"decision\":\"reject\",\"reason\":", stdout);
            mira_json_string(stdout, record->reason);
            fputs(",\"output_path\":null", stdout);
        }
        fputs("}\n", stdout);
    }
}

static void mira_response_header(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, const char* raw_header, size_t raw_header_len) {
    (void)request;
    (void)response;
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !frontend->config->download.server_response || frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET) {
        return;
    }
    (void)fwrite(raw_header, 1, raw_header_len, frontend->diagnostics);
}

static void mira_progress(void* userdata, const BxFetchRequest* request, const BxFetchProgress* progress) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !request || !progress || !frontend->config->download.show_progress || frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET ||
        progress->percent == frontend->last_progress_percent) {
        return;
    }
    frontend->last_progress_percent = progress->percent;
    fprintf(frontend->diagnostics, "\rmira: %3d%% %s", progress->percent, bx_fetch_request_url_for_display(request));
    fflush(frontend->diagnostics);
    frontend->progress_line_active = true;
}

static void mira_retry(void* userdata, const BxFetchPreparedUrl* target, int next_attempt, int max_attempts, int delay_seconds) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET) {
        return;
    }
    if (frontend->progress_line_active) {
        fputc('\n', frontend->diagnostics);
        frontend->progress_line_active = false;
    }
    fprintf(frontend->diagnostics, "mira: retry %d/%d in %d second%s: %s\n", next_attempt, max_attempts, delay_seconds, delay_seconds == 1 ? "" : "s", bx_fetch_prepared_url_display(target));
}

static bool mira_document_error(void* userdata, const BxFetchPreparedUrl* base, const char* path, int depth, const BxFetchDocumentOutcome* outcome) {
    (void)depth;
    MiraRunFrontend* frontend = userdata;
    mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_FILESYSTEM, "failed to process downloaded document", base, path, outcome ? outcome->error_number : EIO);
    return true;
}

static bool mira_link_conversion(void* userdata, const BxFetchDownloadedFileView* download, const BxFetchLinkConversionOutcome* outcome) {
    MiraRunFrontend* frontend = userdata;
    if (!outcome || outcome->failure == BX_FETCH_LINK_CONVERSION_FAILURE_NONE) {
        return true;
    }
    mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_FILESYSTEM, "link conversion failed", NULL, download ? download->local_path : NULL, outcome->error_number);
    return true;
}

static void mira_record_session_failure(MiraRunFrontend* frontend, const BxFetchRunFailure* failure) {
    if (!frontend || !failure)
        return;

    BxFetchErrorClass error_class = BX_FETCH_ERROR_CLASS_INTERNAL;
    const char* summary = "fetch run failed";
    int error_number = failure->error_number;
    switch (failure->stage) {
        case BX_FETCH_RUN_FAILURE_CONFIG:
            error_class = BX_FETCH_ERROR_CLASS_PARSE;
            summary = "invalid fetch configuration";
            break;
        case BX_FETCH_RUN_FAILURE_GLOBAL_INIT:
            error_class = BX_FETCH_ERROR_CLASS_CURL_TRANSPORT;
            summary = "failed to initialize network transport";
            if (failure->setup_error.error_number > 0)
                error_number = failure->setup_error.error_number;
            break;
        case BX_FETCH_RUN_FAILURE_CREATE:
            summary = "failed to initialize fetch run";
            break;
        case BX_FETCH_RUN_FAILURE_LOAD_PUBLICATION:
            error_class = BX_FETCH_ERROR_CLASS_STATE_STORE;
            summary = "failed to load URL mappings";
            break;
        case BX_FETCH_RUN_FAILURE_ADD_SEED:
            error_class = failure->seed.result.status == BX_FETCH_CRAWL_ERROR ? BX_FETCH_ERROR_CLASS_INTERNAL : BX_FETCH_ERROR_CLASS_POLICY;
            summary = "URL rejected by fetch policy";
            break;
        case BX_FETCH_RUN_FAILURE_EXECUTE:
            if (frontend->deferred_error == ENOMEM) {
                summary = "failed to allocate dry-run plan";
                error_number = ENOMEM;
            }
            else {
                error_class = BX_FETCH_ERROR_CLASS_CURL_TRANSPORT;
            }
            break;
        case BX_FETCH_RUN_FAILURE_CONVERT_LINKS:
            error_class = BX_FETCH_ERROR_CLASS_FILESYSTEM;
            summary = "link conversion failed";
            break;
        case BX_FETCH_RUN_FAILURE_SAVE_PUBLICATION:
            error_class = BX_FETCH_ERROR_CLASS_STATE_STORE;
            summary = "failed to save URL mappings";
            break;
        case BX_FETCH_RUN_FAILURE_NONE:
            return;
    }
    mira_run_record_error(frontend, error_class, summary, NULL, NULL, error_number);
}

int bx_mira_run_config(const struct bx_fetch_config* config) {
    if (!config || config->input.url_count <= 0 || !config->input.urls) {
        errno = EINVAL;
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    errno = 0;
    FILE* diagnostics = bx_mira_diagnostics_open(config);
    if (!diagnostics)
        return BX_FETCH_EXIT_FILE_IO;
    MiraRunFrontend frontend_state = {
        .config = config,
        .last_progress_percent = -1,
        .diagnostics = diagnostics,
    };
    if (config->download.dry_run) {
        frontend_state.dry_run_record_count = (size_t)config->input.url_count;
        frontend_state.dry_run_records = calloc(frontend_state.dry_run_record_count, sizeof(*frontend_state.dry_run_records));
        if (!frontend_state.dry_run_records) {
            mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to allocate dry-run plan", NULL, NULL, ENOMEM);
            return bx_mira_diagnostics_finish(config, diagnostics, frontend_state.exit_code);
        }
    }
    BxFetchRunFrontend frontend = {
        .plan_output = mira_plan_output,
        .on_prepare_error = mira_prepare_error,
        .on_submit_error = mira_submit_error,
        .on_completion = mira_completion,
        .on_document_error = mira_document_error,
        .on_link_conversion = mira_link_conversion,
        .on_seed_result = mira_seed_result,
        .transport_observer =
            {
                .on_response_header = mira_response_header,
                .on_progress = mira_progress,
                .userdata = &frontend_state,
            },
        .scheduler_observer =
            {
                .on_retry = mira_retry,
                .userdata = &frontend_state,
            },
        .userdata = &frontend_state,
    };

    bool simple_direct = config->logging.verbosity == BX_FETCH_VERBOSITY_VERBOSE && !config->recursive.recursive && !config->recursive.page_requisites && config->input.url_count == 1;
    if (!config->download.dry_run && !simple_direct)
        fputs("mira: starting downloads\n", frontend_state.diagnostics);

    BxFetchRunFailure failure;
    int execute_result = bx_fetch_run_execute_config(config, &frontend, &failure);
    if (execute_result != 0) {
        bool callback_already_reported_execute_failure =
            (failure.stage == BX_FETCH_RUN_FAILURE_EXECUTE || failure.stage == BX_FETCH_RUN_FAILURE_ADD_SEED) && frontend_state.exit_code != BX_FETCH_EXIT_SUCCESS;
        if (!callback_already_reported_execute_failure)
            mira_record_session_failure(&frontend_state, &failure);
    }
    if (config->download.dry_run) {
        mira_dry_run_records_emit(&frontend_state);
    }
    int exit_code = frontend_state.exit_code;
    mira_dry_run_records_free(&frontend_state);
    return bx_mira_diagnostics_finish(config, diagnostics, exit_code);
}
