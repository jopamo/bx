#include "debug_trace.h"
#include <inttypes.h>

void bx_mira_json_write_string(FILE* stream, const char* value) {
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

void bx_mira_debug_trace_init(MiraDebugTrace* trace, FILE* stream, const struct bx_fetch_config* config) {
    if (!trace)
        return;
    trace->stream = stream;
    trace->sequence = 0;
    trace->enabled = config && config->logging.debug_trace;
}

static void mira_debug_trace_prefix(MiraDebugTrace* trace, const char* layer, const char* action) {
    fprintf(trace->stream,
            "{\"schema_version\":1,\"event\":\"debug-trace\","
            "\"sequence\":%" PRIu64 ",\"layer\":",
            trace->sequence++);
    bx_mira_json_write_string(trace->stream, layer);
    fputs(",\"action\":", trace->stream);
    bx_mira_json_write_string(trace->stream, action);
}

void bx_mira_debug_trace_parse_complete(MiraDebugTrace* trace, const struct bx_fetch_config* config) {
    if (!trace || !trace->enabled || !config)
        return;
    mira_debug_trace_prefix(trace, "parse", "complete");
    fprintf(trace->stream,
            ",\"url_count\":%d,\"has_input_file\":%s,"
            "\"recursive\":%s,\"dry_run\":%s,\"max_threads\":%d}\n",
            config->input.url_count, config->input.input_file ? "true" : "false", config->recursive.recursive ? "true" : "false", config->download.dry_run ? "true" : "false",
            config->download.max_threads);
}

void bx_mira_debug_trace_enqueued(MiraDebugTrace* trace, const char* display_url, const char* output_path, int depth) {
    if (!trace || !trace->enabled)
        return;
    mira_debug_trace_prefix(trace, "scheduler", "enqueued");
    fputs(",\"url\":", trace->stream);
    bx_mira_json_write_string(trace->stream, display_url);
    fputs(",\"output_path\":", trace->stream);
    bx_mira_json_write_string(trace->stream, output_path);
    fprintf(trace->stream, ",\"depth\":%d}\n", depth);
}

void bx_mira_debug_trace_dry_run_decision(MiraDebugTrace* trace, const char* display_url, const char* output_path) {
    if (!trace || !trace->enabled)
        return;
    mira_debug_trace_prefix(trace, "scheduler", "decision");
    fputs(",\"url\":", trace->stream);
    bx_mira_json_write_string(trace->stream, display_url);
    fputs(",\"output_path\":", trace->stream);
    bx_mira_json_write_string(trace->stream, output_path);
    fputs(",\"decision\":\"fetch\",\"reason\":\"dry-run\"}\n", trace->stream);
}

static void mira_debug_trace_transfer_prefix(MiraDebugTrace* trace, const char* layer, const char* action, uint64_t transfer_id, const char* display_url, const char* output_path) {
    mira_debug_trace_prefix(trace, layer, action);
    fprintf(trace->stream, ",\"transfer_id\":%" PRIu64 ",\"url\":", transfer_id);
    bx_mira_json_write_string(trace->stream, display_url);
    fputs(",\"output_path\":", trace->stream);
    bx_mira_json_write_string(trace->stream, output_path);
}

static const char* mira_submit_failure_reason(const BxFetchRunTransferObservation* observation) {
    if (observation->failure == BX_FETCH_RUN_SUBMIT_FAILURE_PREPARE) {
        switch (observation->prepare_failure) {
            case BX_FETCH_PREPARE_FAILURE_REQUEST:
                return "request-new";
            case BX_FETCH_PREPARE_FAILURE_REQUEST_BODY:
                return "open-request-body";
            case BX_FETCH_PREPARE_FAILURE_WRITER:
                return "open-writer";
            case BX_FETCH_PREPARE_FAILURE_PROTOCOL_POLICY:
                return "protocol-policy";
            case BX_FETCH_PREPARE_FAILURE_REQUEST_HEADER:
                return "request-header-policy";
            case BX_FETCH_PREPARE_FAILURE_METADATA:
                return "read-metadata";
            case BX_FETCH_PREPARE_FAILURE_INVALID_ARGUMENT:
                return "invalid-argument";
            case BX_FETCH_PREPARE_FAILURE_NONE:
            default:
                return "prepare-candidate";
        }
    }
    switch (observation->failure) {
        case BX_FETCH_RUN_SUBMIT_FAILURE_STATE:
            return "alloc-transfer-context";
        case BX_FETCH_RUN_SUBMIT_FAILURE_ENGINE:
            return "net-engine-submit";
        case BX_FETCH_RUN_SUBMIT_FAILURE_NONE:
        default:
            return "unspecified";
    }
}

static const char* mira_transfer_result_reason(BxFetchError result) {
    switch (result) {
        case BX_FETCH_ERROR_HTTP:
            return "http-status";
        case BX_FETCH_ERROR_IO:
            return "io";
        case BX_FETCH_ERROR_SSL:
            return "tls";
        case BX_FETCH_ERROR_TIMEOUT:
            return "timeout";
        case BX_FETCH_ERROR_NETWORK:
            return "network";
        case BX_FETCH_ERROR_MEMORY:
            return "memory";
        case BX_FETCH_ERROR_INVALID_ARGUMENT:
            return "invalid-argument";
        case BX_FETCH_ERROR_INTERNAL:
            return "internal";
        case BX_FETCH_ERROR_UNSUPPORTED:
            return "unsupported";
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
            return "resource-limit";
        case BX_FETCH_ERROR_CANCELLED:
            return "cancelled";
        case BX_FETCH_OK:
        default:
            return "none";
    }
}

void bx_mira_debug_trace_transfer_observation(MiraDebugTrace* trace, const BxFetchRunTransferObservation* observation) {
    if (!trace || !trace->enabled || !observation || !observation->target || !observation->output_path)
        return;

    const char* layer = observation->event == BX_FETCH_RUN_TRANSFER_DISPATCH ? "scheduler" : "transfer";
    const char* action = NULL;
    switch (observation->event) {
        case BX_FETCH_RUN_TRANSFER_DISPATCH:
            action = "dispatch";
            break;
        case BX_FETCH_RUN_TRANSFER_SUBMIT:
            action = "submit";
            break;
        case BX_FETCH_RUN_TRANSFER_SUBMITTED:
            action = "submitted";
            break;
        case BX_FETCH_RUN_TRANSFER_SUBMIT_FAILED:
            action = "submit-failed";
            break;
        default:
            return;
    }
    mira_debug_trace_transfer_prefix(trace, layer, action, observation->transfer_id, bx_fetch_prepared_url_display(observation->target), observation->output_path);
    if (observation->event == BX_FETCH_RUN_TRANSFER_DISPATCH)
        fprintf(trace->stream, ",\"depth\":%d", observation->depth);
    if (observation->event == BX_FETCH_RUN_TRANSFER_SUBMIT_FAILED) {
        fputs(",\"reason\":", trace->stream);
        bx_mira_json_write_string(trace->stream, mira_submit_failure_reason(observation));
    }
    if (observation->failure == BX_FETCH_RUN_SUBMIT_FAILURE_PREPARE || observation->failure == BX_FETCH_RUN_SUBMIT_FAILURE_STATE)
        fputs("}\n", trace->stream);
    else
        fprintf(trace->stream, ",\"attempt\":%d,\"max_attempts\":%d}\n", observation->attempt, observation->max_attempts);
}

void bx_mira_debug_trace_completion(MiraDebugTrace* trace, const struct bx_fetch_config* config, const BxFetchRunCompletion* completion) {
    if (!trace || !trace->enabled || !config || !completion || !completion->transfer || !completion->transfer->request || !completion->transfer->output_path)
        return;

    const BxFetchTransferCompletion* transfer = completion->transfer;
    const BxFetchResponse* response = transfer->response;
    const char* display_url = bx_fetch_request_url_for_display(transfer->request);
    int status = response ? response->status_code : 0;
    BxFetchOutputState output_state = response ? response->output_state : BX_FETCH_OUTPUT_STATE_NONE;
    bool committed = output_state == BX_FETCH_OUTPUT_STATE_COMMITTED || output_state == BX_FETCH_OUTPUT_STATE_METADATA_COMMITTED || output_state == BX_FETCH_OUTPUT_STATE_UNCHANGED;
    if (config->download.metadata_sidecars && output_state == BX_FETCH_OUTPUT_STATE_COMMITTED && (status == 200 || status == 206)) {
        mira_debug_trace_transfer_prefix(trace, "commit", "metadata-staged", completion->transfer_id, display_url, transfer->output_path);
        fputs("}\n", trace->stream);
    }

    mira_debug_trace_transfer_prefix(trace, "transfer", "complete", completion->transfer_id, display_url, transfer->output_path);
    fputs(",\"status\":", trace->stream);
    if (status > 0)
        fprintf(trace->stream, "%d", status);
    else
        fputs("null", trace->stream);
    fputs(",\"result\":", trace->stream);
    bx_mira_json_write_string(trace->stream, bx_fetch_error_string(transfer->result));
    fprintf(trace->stream,
            ",\"result_code\":%d,\"attempt\":%d,\"max_attempts\":%d,"
            "\"retry_scheduled\":%s}\n",
            (int)transfer->result, completion->attempt, completion->max_attempts, completion->retry_scheduled ? "true" : "false");

    if (completion->retry_scheduled) {
        mira_debug_trace_transfer_prefix(trace, "retry", "decision", completion->transfer_id, display_url, transfer->output_path);
        fputs(",\"status\":", trace->stream);
        if (status > 0)
            fprintf(trace->stream, "%d", status);
        else
            fputs("null", trace->stream);
        fputs(",\"decision\":\"scheduled\",\"reason\":", trace->stream);
        bx_mira_json_write_string(trace->stream, mira_transfer_result_reason(transfer->result));
        fprintf(trace->stream, ",\"attempt\":%d,\"max_attempts\":%d}\n", completion->attempt, completion->max_attempts);
    }

    mira_debug_trace_transfer_prefix(trace, "commit", committed ? "complete" : "aborted", completion->transfer_id, display_url, transfer->output_path);
    fputs(",\"status\":", trace->stream);
    if (status > 0)
        fprintf(trace->stream, "%d", status);
    else
        fputs("null", trace->stream);
    fputs(",\"result\":", trace->stream);
    bx_mira_json_write_string(trace->stream, bx_fetch_error_string(transfer->result));
    fputs(",\"reason\":", trace->stream);
    if (committed)
        fputs("null", trace->stream);
    else {
        const char* reason = config->download.spider                            ? "spider"
                             : completion->retry_scheduled                      ? "retry-scheduled"
                             : transfer->result == BX_FETCH_OK && status == 304 ? "not-modified"
                                                                                : mira_transfer_result_reason(transfer->result);
        bx_mira_json_write_string(trace->stream, reason);
    }
    fputs("}\n", trace->stream);
}
