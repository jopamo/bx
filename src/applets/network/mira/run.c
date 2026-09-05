#define _GNU_SOURCE
#include "debug_trace.h"
#include "logging.h"
#include "mira.h"
#include "lib/fetch/error.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/filter.h"
#include "lib/fetch/pathmap.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/run.h"
#include "lib/path_quote.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    BxFetchCrawlEnqueueStatus status;
    bool observed;
    bool no_clobber_skip;
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
    size_t dry_run_record_capacity;
    size_t next_plan_index;
    int deferred_error;
    FILE* diagnostics;
    FILE* rejected_log;
    MiraDebugTrace debug_trace;
} MiraRunFrontend;

static MiraDryRunRecord* mira_dry_run_record(MiraRunFrontend* frontend, int index) {
    if (!frontend || index < 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t required = (size_t)index + 1u;
    if (required > BX_FETCH_URL_STATE_MAX_ENTRIES) {
        errno = EFBIG;
        return NULL;
    }
    if (required > frontend->dry_run_record_capacity) {
        size_t next_capacity = frontend->dry_run_record_capacity ? frontend->dry_run_record_capacity : 16u;
        while (next_capacity < required) {
            if (next_capacity > BX_FETCH_URL_STATE_MAX_ENTRIES / 2u) {
                next_capacity = BX_FETCH_URL_STATE_MAX_ENTRIES;
                break;
            }
            next_capacity *= 2u;
        }
        MiraDryRunRecord* grown = realloc(frontend->dry_run_records, next_capacity * sizeof(*grown));
        if (!grown)
            return NULL;
        memset(grown + frontend->dry_run_record_capacity, 0, (next_capacity - frontend->dry_run_record_capacity) * sizeof(*grown));
        frontend->dry_run_records = grown;
        frontend->dry_run_record_capacity = next_capacity;
    }
    if (frontend->dry_run_record_count < required)
        frontend->dry_run_record_count = required;
    return &frontend->dry_run_records[index];
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

static MiraDryRunRecord* mira_dry_run_plan_record(MiraRunFrontend* frontend,
                                                  const BxFetchPreparedUrl* target,
                                                  const char* output_path) {
    while (frontend->next_plan_index < frontend->dry_run_record_count &&
           frontend->dry_run_records[frontend->next_plan_index].status != BX_FETCH_CRAWL_ENQUEUED) {
        frontend->next_plan_index++;
    }
    if (frontend->next_plan_index >= frontend->dry_run_record_count) {
        errno = EPROTO;
        return NULL;
    }

    MiraDryRunRecord* record = &frontend->dry_run_records[frontend->next_plan_index++];
    record->display_url = strdup(bx_fetch_prepared_url_display(target));
    record->normalized_url = strdup(bx_fetch_prepared_url_display(target));
    record->output_path = strdup(output_path);
    if (!record->display_url || !record->normalized_url || !record->output_path) {
        frontend->deferred_error = ENOMEM;
        errno = ENOMEM;
        return NULL;
    }
    return record;
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

    if (frontend->config->logging.debug_trace) {
        bx_mira_debug_trace_enqueued(
            &frontend->debug_trace, bx_fetch_prepared_url_display(target), *output_path_out, depth);
    }

    return 0;
}

static int mira_output_observation(void* userdata, const BxFetchRunOutputObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !observation)
        return -1;
    if (observation->decision == BX_FETCH_RUN_OUTPUT_INSPECTION_ERROR) {
        frontend->deferred_error = observation->error_number ? observation->error_number : EIO;
        mira_run_record_error(frontend,
                              BX_FETCH_ERROR_CLASS_FILESYSTEM,
                              "failed to inspect no-clobber destination",
                              observation->target,
                              observation->output_path,
                              frontend->deferred_error);
        return 0;
    }

    MiraDryRunRecord* record = NULL;
    if (frontend->config->download.dry_run) {
        record = mira_dry_run_plan_record(frontend, observation->target, observation->output_path);
        if (!record) {
            frontend->deferred_error = errno ? errno : ENOMEM;
            return -1;
        }
    }
    if (observation->decision == BX_FETCH_RUN_OUTPUT_SKIP_NO_CLOBBER) {
        if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
            char* quoted = bx_path_quote_dup(observation->output_path, BX_PATH_QUOTE_ESCAPE);
            fprintf(frontend->diagnostics,
                    "mira: skipping existing file due to --no-clobber: %s (%s)\n",
                    quoted ? quoted : "(path unavailable)",
                    bx_fetch_prepared_url_display(observation->target));
            free(quoted);
        }
        bx_mira_debug_trace_no_clobber_decision(
            &frontend->debug_trace, bx_fetch_prepared_url_display(observation->target), observation->output_path);
        if (record)
            record->no_clobber_skip = true;
        return 0;
    }

    if (record)
        bx_mira_debug_trace_dry_run_decision(&frontend->debug_trace, record->display_url, record->output_path);
    return 0;
}

static void mira_response_name(void* userdata, const BxFetchRunResponseNameObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !observation ||
        observation->decision == BX_FETCH_RUN_RESPONSE_NAME_ADJUSTED ||
        frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET) {
        return;
    }

    char* original = observation->original_path ? bx_path_quote_dup(observation->original_path, BX_PATH_QUOTE_ESCAPE) : NULL;
    char* candidate = observation->candidate_path ? bx_path_quote_dup(observation->candidate_path, BX_PATH_QUOTE_ESCAPE) : NULL;
    const char* original_text = original ? original : "(path unavailable)";
    const char* candidate_text = candidate ? candidate : "(path unavailable)";
    switch (observation->decision) {
        case BX_FETCH_RUN_RESPONSE_NAME_KEEP_NO_CLOBBER:
            fprintf(frontend->diagnostics,
                    "mira: keeping %s due to --no-clobber (target exists: %s)\n",
                    original_text,
                    candidate_text);
            break;
        case BX_FETCH_RUN_RESPONSE_NAME_KEEP_EXISTING:
            fprintf(frontend->diagnostics,
                    "mira: keeping %s because server-selected output path already exists: %s\n",
                    original_text,
                    candidate_text);
            break;
        case BX_FETCH_RUN_RESPONSE_NAME_FAILED:
            fprintf(frontend->diagnostics,
                    "mira: failed to stage final output path %s: %s\n",
                    candidate_text,
                    strerror(observation->error_number > 0 ? observation->error_number : EIO));
            break;
        case BX_FETCH_RUN_RESPONSE_NAME_ADJUSTED:
            break;
    }
    free(candidate);
    free(original);
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
    if (frontend->progress_line_active) {
        fputc('\n', frontend->diagnostics);
        frontend->progress_line_active = false;
    }
    frontend->last_progress_percent = -1;
    bx_mira_debug_trace_completion(&frontend->debug_trace, frontend->config, completion);
    if (completion->retry_scheduled)
        return 0;
    if (completion->transfer->result == BX_FETCH_OK) {
        if (frontend->config->logging.verbosity == BX_FETCH_VERBOSITY_VERBOSE && !frontend->config->download.spider) {
            fprintf(frontend->diagnostics, "mira: saved %s\n", completion->transfer->output_path);
        }
        return 0;
    }
    /*
     * A rejected redirect is represented as transport cancellation so no body
     * can be committed. Policy was already observed at the redirect boundary;
     * it is not a transfer error and must not produce a second diagnostic.
     */
    if (completion->redirect_rejected && completion->transfer->result == BX_FETCH_ERROR_CANCELLED)
        return 0;

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
        char summary[64];
        const char* error_summary = "transfer failed";
        if (status >= 400 && status < 600) {
            snprintf(summary, sizeof(summary), "HTTP status %d", status);
            error_summary = summary;
        }
        BxFetchStructuredError error = {
            .class_id = bx_fetch_error_class_for_exit_code(exit_code),
            .summary = error_summary,
            .url = request ? bx_fetch_request_url_for_display(request) : NULL,
            .path = completion->transfer->output_path,
            .http_status = status > 0 ? status : -1,
            .curl_code = response && response->error_code != 0 ? response->error_code : -1,
            .error_number = response && response->error_number > 0 ? response->error_number : -1,
            .retryable = completion->transfer->retryable_hint && completion->attempt < completion->max_attempts,
            .attempt = completion->attempt,
            .max_attempts = completion->max_attempts,
        };
        bx_fetch_error_emit_structured(frontend->diagnostics, &error);
    }
    return 0;
}

static void mira_transfer_observation(void* userdata, const BxFetchRunTransferObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (frontend)
        bx_mira_debug_trace_transfer_observation(&frontend->debug_trace, observation);
}

static void mira_log_rejected_target(MiraRunFrontend* frontend,
                                     const BxFetchPreparedUrl* target,
                                     BxFetchFilterDecision decision) {
    if (!frontend || !frontend->rejected_log || decision == FILTER_DECISION_ACCEPT)
        return;
    const char* reason = bx_fetch_filter_decision_reason(decision);
    fprintf(frontend->rejected_log,
            "%s\t%s\n",
            target ? bx_fetch_prepared_url_display(target) : BX_FETCH_URL_DISPLAY_REDACTED,
            reason ? reason : "unspecified");
    fflush(frontend->rejected_log);
}

static bool mira_seed_result(void* userdata, const BxFetchRunSeedObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !observation)
        return false;
    if (frontend->config->download.dry_run && observation->index >= 0) {
        MiraDryRunRecord* record = mira_dry_run_record(frontend, observation->index);
        if (!record) {
            frontend->deferred_error = errno ? errno : ENOMEM;
            mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to allocate dry-run plan", NULL, NULL, frontend->deferred_error);
            return false;
        }
        record->observed = true;
        record->status = observation->result.status;
        if (observation->result.status == BX_FETCH_CRAWL_REJECTED) {
            const char* display_url = observation->target ? bx_fetch_prepared_url_display(observation->target) : BX_FETCH_URL_DISPLAY_REDACTED;
            record->display_url = strdup(display_url);
            record->normalized_url = observation->target ? strdup(display_url) : NULL;
            const char* reason = bx_fetch_filter_decision_reason(observation->result.filter_decision);
            record->reason = strdup(reason ? reason : "unspecified");
            if (!record->display_url || (observation->target && !record->normalized_url) || !record->reason) {
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
        mira_run_record_error_url(frontend, error_class, summary, "[URL redacted]", observation->source_path, -1);
        return false;
    }
    if (observation->result.status != BX_FETCH_CRAWL_REJECTED)
        return false;
    mira_log_rejected_target(frontend, observation->target, observation->result.filter_decision);
    if (!observation->target) {
        bool unsupported = observation->result.filter_decision == FILTER_DECISION_UNSUPPORTED_PROTOCOL;
        const char* summary = unsupported ? "initial URL uses an unsupported protocol" : "invalid initial URL";
        BxFetchErrorClass error_class = unsupported ? BX_FETCH_ERROR_CLASS_CURL_TRANSPORT : BX_FETCH_ERROR_CLASS_POLICY;
        mira_run_record_error_url(frontend, error_class, summary, BX_FETCH_URL_DISPLAY_REDACTED, observation->source_path, -1);
        return true;
    }
    const char* reason = bx_fetch_filter_decision_reason(observation->result.filter_decision);
    if (observation->result.filter_decision == FILTER_DECISION_URL_CREDENTIALS) {
        mira_run_record_error(frontend, BX_FETCH_ERROR_CLASS_POLICY, "URL credentials are disabled by --paranoid", observation->target, observation->source_path, -1);
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

static void mira_discovered_link(void* userdata, const BxFetchRunDiscoveredLinkObservation* observation) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !observation || observation->result.status != BX_FETCH_CRAWL_REJECTED)
        return;
    mira_log_rejected_target(frontend, observation->target, observation->result.filter_decision);
}

static bool mira_redirect(void* userdata, const BxFetchPreparedUrl* target, BxFetchFilterDecision shared_decision) {
    MiraRunFrontend* frontend = userdata;
    mira_log_rejected_target(frontend, target, shared_decision);
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
    frontend->dry_run_record_capacity = 0;
}

static void mira_record_input_failure(MiraRunFrontend* frontend, const BxFetchInputOutcome* outcome) {
    if (!frontend || !outcome)
        return;

    char summary[192];
    BxFetchErrorClass error_class = BX_FETCH_ERROR_CLASS_FILESYSTEM;
    bool emit_text = true;
    switch (outcome->kind) {
        case BX_FETCH_INPUT_FAILURE_OPEN:
            snprintf(summary, sizeof(summary), "failed to open input file");
            break;
        case BX_FETCH_INPUT_FAILURE_READ:
            snprintf(summary, sizeof(summary), "failed to read input file at line %zu", outcome->line_number);
            break;
        case BX_FETCH_INPUT_FAILURE_LINE_TOO_LONG:
            snprintf(summary, sizeof(summary), "input file line %zu exceeds " BX_FETCH_URL_LIMIT_TEXT " byte limit", outcome->line_number);
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            emit_text = false;
            break;
        case BX_FETCH_INPUT_FAILURE_INVALID_CONTROL:
            snprintf(summary, sizeof(summary), "input file line %zu contains an invalid control byte", outcome->line_number);
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            break;
        case BX_FETCH_INPUT_FAILURE_FILE_TOO_LARGE:
            snprintf(summary, sizeof(summary), "input file exceeds " BX_FETCH_INPUT_FILE_LIMIT_TEXT " byte limit");
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            emit_text = false;
            break;
        case BX_FETCH_INPUT_FAILURE_HTML_TOO_LARGE:
            snprintf(summary, sizeof(summary), "HTML input file exceeds " BX_FETCH_DOCUMENT_PARSE_LIMIT_TEXT " parser limit");
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            emit_text = false;
            break;
        case BX_FETCH_INPUT_FAILURE_HTML_PARSE:
            snprintf(summary, sizeof(summary), "failed to parse HTML input file");
            error_class = BX_FETCH_ERROR_CLASS_PARSE;
            emit_text = false;
            break;
        case BX_FETCH_INPUT_FAILURE_BASE_URL:
            snprintf(summary, sizeof(summary), "invalid --base URL");
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            break;
        case BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT:
            snprintf(summary, sizeof(summary), "input file URL state exceeds the bounded URL-state contract");
            error_class = BX_FETCH_ERROR_CLASS_POLICY;
            emit_text = false;
            break;
        case BX_FETCH_INPUT_FAILURE_MEMORY:
            snprintf(summary, sizeof(summary), "failed to allocate bounded input-file state");
            error_class = BX_FETCH_ERROR_CLASS_INTERNAL;
            break;
        case BX_FETCH_INPUT_FAILURE_NONE:
        default:
            snprintf(summary, sizeof(summary), "failed to read input file");
            error_class = BX_FETCH_ERROR_CLASS_INTERNAL;
            break;
    }

    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, bx_fetch_exit_code_for_error_class(error_class, -1));
    if (emit_text) {
        char* quoted = bx_path_quote_dup(frontend->config->input.input_file, BX_PATH_QUOTE_ESCAPE);
        fprintf(frontend->diagnostics, "mira: %s%s%s\n", summary, quoted ? ": " : "", quoted ? quoted : "");
        free(quoted);
    }
    if (frontend->config->logging.structured_errors) {
        bx_fetch_error_emit_simple(frontend->diagnostics,
                                   error_class,
                                   summary,
                                   NULL,
                                   frontend->config->input.input_file,
                                   -1,
                                   outcome->error_number);
    }
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
        bx_mira_json_write_string(stdout, record->display_url);
        fputs(",\"normalized_url\":", stdout);
        if (record->normalized_url)
            bx_mira_json_write_string(stdout, record->normalized_url);
        else
            fputs("null", stdout);
        if (record->no_clobber_skip) {
            fputs(",\"decision\":\"skip\",\"reason\":\"no-clobber\",\"output_path\":", stdout);
            bx_mira_json_write_string(stdout, record->output_path);
        }
        else if (record->status == BX_FETCH_CRAWL_ENQUEUED) {
            fputs(",\"decision\":\"fetch\",\"reason\":null,\"output_path\":", stdout);
            bx_mira_json_write_string(stdout, record->output_path);
        }
        else {
            fputs(",\"decision\":\"reject\",\"reason\":", stdout);
            bx_mira_json_write_string(stdout, record->reason);
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
        case BX_FETCH_RUN_FAILURE_LOAD_INPUT:
            mira_record_input_failure(frontend, &failure->input);
            return;
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
    if (!config || (config->input.url_count <= 0 && !config->input.input_file) || (config->input.url_count > 0 && !config->input.urls)) {
        errno = EINVAL;
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    errno = 0;
    FILE* diagnostics = bx_mira_diagnostics_open(config);
    if (!diagnostics)
        return BX_FETCH_EXIT_FILE_IO;
    FILE* rejected_log = NULL;
    if (bx_mira_rejected_log_open(config, &rejected_log) != 0)
        return bx_mira_diagnostics_finish(config, diagnostics, BX_FETCH_EXIT_FILE_IO);
    MiraRunFrontend frontend_state = {
        .config = config,
        .last_progress_percent = -1,
        .diagnostics = diagnostics,
        .rejected_log = rejected_log,
    };
    bx_mira_debug_trace_init(&frontend_state.debug_trace, diagnostics, config);
    bx_mira_debug_trace_parse_complete(&frontend_state.debug_trace, config);
    if (config->download.dry_run && config->input.url_count > 0) {
        frontend_state.dry_run_record_count = (size_t)config->input.url_count;
        frontend_state.dry_run_record_capacity = frontend_state.dry_run_record_count;
        frontend_state.dry_run_records = calloc(frontend_state.dry_run_record_count, sizeof(*frontend_state.dry_run_records));
        if (!frontend_state.dry_run_records) {
            mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to allocate dry-run plan", NULL, NULL, ENOMEM);
            int exit_code = bx_mira_rejected_log_finish(config, rejected_log, frontend_state.exit_code);
            return bx_mira_diagnostics_finish(config, diagnostics, exit_code);
        }
    }
    BxFetchRunFrontend frontend = {
        .plan_output = mira_plan_output,
        .on_prepare_error = mira_prepare_error,
        .on_submit_error = mira_submit_error,
        .on_completion = mira_completion,
        .allow_redirect = mira_redirect,
        .on_discovered_link = mira_discovered_link,
        .on_document_error = mira_document_error,
        .on_link_conversion = mira_link_conversion,
        .on_seed_result = mira_seed_result,
        .on_output_observation = (config->download.dry_run || config->download.no_clobber) ? mira_output_observation : NULL,
        .on_response_name = config->http.adjust_extension ? mira_response_name : NULL,
        .on_transfer_observation = config->logging.debug_trace ? mira_transfer_observation : NULL,
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
    exit_code = bx_mira_rejected_log_finish(config, rejected_log, exit_code);
    return bx_mira_diagnostics_finish(config, diagnostics, exit_code);
}
