#define _GNU_SOURCE
#include "mira.h"
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

static void mira_run_record_error(MiraRunFrontend* frontend, BxFetchErrorClass error_class, const char* summary, const BxFetchPreparedUrl* target, const char* path, int error_number) {
    if (!frontend)
        return;
    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, bx_fetch_exit_code_for_error_class(error_class, -1));
    if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET)
        fprintf(stderr, "mira: %s\n", summary);
    if (frontend->config->logging.structured_errors) {
        bx_fetch_error_emit_simple(stderr, error_class, summary, target ? bx_fetch_prepared_url_display(target) : NULL, path, -1, error_number);
    }
}

static int mira_plan_output(void* userdata, const BxFetchPreparedUrl* target, int depth, char** output_path_out) {
    MiraRunFrontend* frontend = userdata;
    if (!frontend || !target || !output_path_out || depth != 0) {
        errno = EINVAL;
        return -1;
    }

    if (frontend->config->download.output_document)
        *output_path_out = strdup(frontend->config->download.output_document);
    else
        *output_path_out = bx_fetch_pathmap_canonical_url_to_local(bx_fetch_prepared_url_transport(target), frontend->config);
    if (!*output_path_out)
        return -1;

    if (frontend->config->download.dry_run) {
        fputs("{\"schema_version\":1,\"event\":\"dry-run-plan\",\"sequence\":0,\"url\":", stdout);
        mira_json_string(stdout, bx_fetch_prepared_url_display(target));
        fputs(",\"normalized_url\":", stdout);
        mira_json_string(stdout, bx_fetch_prepared_url_transport(target));
        fputs(",\"decision\":\"fetch\",\"reason\":null,\"output_path\":", stdout);
        mira_json_string(stdout, *output_path_out);
        fputs("}\n", stdout);
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
    if (completion->transfer->result == BX_FETCH_OK)
        return 0;

    const BxFetchResponse* response = completion->transfer->response;
    int status = response ? response->status_code : 0;
    BxFetchTransportErrorKind transport_kind = response ? response->transport_error_kind : BX_FETCH_TRANSPORT_ERROR_NONE;
    int exit_code = bx_fetch_exit_code_for_transfer_failure(status, transport_kind, completion->transfer->result);
    frontend->exit_code = bx_fetch_exit_combine(frontend->exit_code, exit_code);
    if (frontend->config->logging.verbosity != BX_FETCH_VERBOSITY_QUIET) {
        fprintf(stderr, "mira: transfer failed: %s\n", bx_fetch_error_string(completion->transfer->result));
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
        bx_fetch_error_emit_structured(stderr, &error);
    }
    return 0;
}

int bx_mira_run_config(const struct bx_fetch_config* config) {
    if (!config || config->input.url_count != 1 || !config->input.urls || !config->input.urls[0]) {
        errno = EINVAL;
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    MiraRunFrontend frontend_state = {
        .config = config,
    };
    BxFetchRunFrontend frontend = {
        .plan_output = mira_plan_output,
        .on_prepare_error = mira_prepare_error,
        .on_submit_error = mira_submit_error,
        .on_completion = mira_completion,
        .userdata = &frontend_state,
    };

    bool global_initialized = false;
    BxFetchNetSetupError setup_error = {0};
    if (!config->download.dry_run) {
        if (config->logging.verbosity == BX_FETCH_VERBOSITY_QUIET)
            fputs("mira: starting downloads\n", stderr);
        if (bx_fetch_global_init(config, &setup_error) != 0) {
            mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_CURL_TRANSPORT, "failed to initialize network transport", NULL, NULL,
                                  setup_error.error_number ? setup_error.error_number : errno);
            return frontend_state.exit_code ? frontend_state.exit_code : BX_FETCH_EXIT_NETWORK;
        }
        global_initialized = true;
    }

    BxFetchRun* run = bx_fetch_run_new(config, &frontend);
    if (!run) {
        mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_INTERNAL, "failed to initialize fetch run", NULL, NULL, errno);
        goto cleanup;
    }
    if (bx_fetch_run_load_publication(run) != 0) {
        mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_STATE_STORE, "failed to load URL mappings", NULL, NULL, errno);
        goto cleanup;
    }

    BxFetchCrawlEnqueueResult enqueue = bx_fetch_run_add_seed(run, config->input.urls[0]);
    if (enqueue.status != BX_FETCH_CRAWL_ENQUEUED) {
        BxFetchErrorClass error_class = enqueue.status == BX_FETCH_CRAWL_ERROR ? BX_FETCH_ERROR_CLASS_INTERNAL : BX_FETCH_ERROR_CLASS_POLICY;
        mira_run_record_error(&frontend_state, error_class, "URL rejected by fetch policy", NULL, NULL, errno);
        goto cleanup;
    }
    if (bx_fetch_run_execute(run) != 0) {
        if (!frontend_state.exit_code)
            mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_CURL_TRANSPORT, "fetch run failed", NULL, NULL, errno);
        goto cleanup;
    }
    if (bx_fetch_run_convert_links(run) != 0) {
        mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_FILESYSTEM, "link conversion failed", NULL, NULL, errno);
        goto cleanup;
    }
    if (bx_fetch_run_save_publication(run) != 0)
        mira_run_record_error(&frontend_state, BX_FETCH_ERROR_CLASS_STATE_STORE, "failed to save URL mappings", NULL, NULL, errno);

cleanup:
    bx_fetch_run_free(run);
    if (global_initialized)
        bx_fetch_global_cleanup();
    return frontend_state.exit_code;
}
