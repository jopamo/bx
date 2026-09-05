#define _GNU_SOURCE
#include "lib/fetch/transfer_prepare.h"
#include "lib/fetch/metadata.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

struct BxFetchTransferCandidate {
    const struct bx_fetch_config* cfg;
    BxFetchRequest* request;
    BxFetchWriter* writer;
};

typedef struct {
    const struct bx_fetch_config* cfg;
    BxFetchTransferHeadersCallback headers_callback;
    BxFetchTransferCompletionCallback completion_callback;
    void* userdata;
    char* output_path;
} CandidateCallbacks;

static BxFetchTransferCandidate* prepare_failure(BxFetchTransferCandidate* candidate,
                                                 BxFetchPrepareError* error,
                                                 BxFetchPrepareFailureKind kind,
                                                 int error_number,
                                                 BxFetchProtocolDecision protocol_decision,
                                                 BxFetchRequestBodyResult body_result) {
    if (candidate) {
        if (candidate->writer)
            bx_fetch_writer_abort(candidate->writer);
        bx_fetch_request_free(candidate->request);
        free(candidate);
    }
    if (error) {
        *error = (BxFetchPrepareError){
            .kind = kind,
            .error_number = error_number,
            .protocol_decision = protocol_decision,
            .body_result = body_result,
        };
    }
    errno = error_number;
    return NULL;
}

static const char* configured_method(const struct bx_fetch_config* cfg) {
    if (cfg->http.method && cfg->http.method[0] != '\0')
        return cfg->http.method;
    if (cfg->http.post_data || cfg->http.post_file)
        return "POST";
    return "GET";
}

static BxFetchRequestBodyResult apply_request_body(const struct bx_fetch_config* cfg, BxFetchRequest* request) {
    if (cfg->http.post_data) {
        size_t length = strlen(cfg->http.post_data);
        return bx_fetch_request_set_body(request, cfg->http.post_data, length);
    }
    if (cfg->http.post_file)
        return bx_fetch_request_set_body_file(request, cfg->http.post_file);
    return BX_FETCH_REQUEST_BODY_OK;
}

static bool method_allows_existing_output_policy(const char* method) {
    return method && strcasecmp(method, "GET") == 0;
}

static int add_header(BxFetchRequest* request, const char* name, const char* value) {
    if (bx_fetch_request_add_header(request, name, value) == 0)
        return 0;
    if (errno == 0)
        errno = EINVAL;
    return -1;
}

static int add_resume_header(BxFetchRequest* request, const BxFetchWriter* writer) {
    uint64_t resume_from = bx_fetch_writer_candidate_size(writer);
    if (resume_from == 0)
        return 0;

    char range[64];
    int length = snprintf(range, sizeof(range), "bytes=%" PRIu64 "-", resume_from);
    if (length < 0 || (size_t)length >= sizeof(range)) {
        errno = EOVERFLOW;
        return -1;
    }
    return add_header(request, "Range", range);
}

typedef enum {
    CONDITIONAL_HEADERS_OK = 0,
    CONDITIONAL_HEADERS_METADATA_FAILURE,
    CONDITIONAL_HEADERS_REQUEST_FAILURE,
} ConditionalHeadersResult;

static ConditionalHeadersResult add_conditional_headers(const struct bx_fetch_config* cfg, const BxFetchWriter* writer, BxFetchRequest* request) {
    BxFetchMetadata metadata = {0};
    if (bx_fetch_writer_load_original_metadata(writer, &metadata) != 0)
        return CONDITIONAL_HEADERS_METADATA_FAILURE;

    bool have_modified_since = false;
    int result = 0;
    if (metadata.etag && metadata.etag[0] != '\0') {
        result = add_header(request, "If-None-Match", metadata.etag);
    }
    if (result == 0 && !cfg->download.no_if_modified_since && metadata.last_modified && metadata.last_modified[0] != '\0') {
        result = add_header(request, "If-Modified-Since", metadata.last_modified);
        have_modified_since = result == 0;
    }
    bx_fetch_metadata_clear(&metadata);
    if (result != 0)
        return CONDITIONAL_HEADERS_REQUEST_FAILURE;
    if (cfg->download.no_if_modified_since || have_modified_since)
        return CONDITIONAL_HEADERS_OK;

    time_t mtime;
    if (!bx_fetch_writer_original_mtime(writer, &mtime))
        return CONDITIONAL_HEADERS_OK;

    struct tm broken_down;
    if (!gmtime_r(&mtime, &broken_down))
        return CONDITIONAL_HEADERS_METADATA_FAILURE;
    char timestamp[128];
    if (strftime(timestamp, sizeof(timestamp), "%a, %d %b %Y %H:%M:%S GMT", &broken_down) == 0) {
        errno = EOVERFLOW;
        return CONDITIONAL_HEADERS_METADATA_FAILURE;
    }
    return add_header(request, "If-Modified-Since", timestamp) == 0 ? CONDITIONAL_HEADERS_OK : CONDITIONAL_HEADERS_REQUEST_FAILURE;
}

BxFetchTransferCandidate* bx_fetch_transfer_candidate_prepare(const struct bx_fetch_config* cfg, const BxFetchPreparedUrl* target, const char* output_path, BxFetchPrepareError* error) {
    if (error)
        *error = (BxFetchPrepareError){0};
    if (!cfg || !target || !output_path || output_path[0] == '\0') {
        return prepare_failure(NULL, error, BX_FETCH_PREPARE_FAILURE_INVALID_ARGUMENT, EINVAL, BX_FETCH_PROTOCOL_DECISION_INVALID_URL, BX_FETCH_REQUEST_BODY_OK);
    }

    BxFetchProtocolDecision protocol_decision = bx_fetch_prepared_url_policy(target, cfg->https.https_only);
    if (protocol_decision != BX_FETCH_PROTOCOL_DECISION_ALLOW) {
        return prepare_failure(NULL, error, BX_FETCH_PREPARE_FAILURE_PROTOCOL_POLICY, EPROTONOSUPPORT, protocol_decision, BX_FETCH_REQUEST_BODY_OK);
    }

    BxFetchTransferCandidate* candidate = calloc(1, sizeof(*candidate));
    if (!candidate) {
        return prepare_failure(NULL, error, BX_FETCH_PREPARE_FAILURE_REQUEST, ENOMEM, protocol_decision, BX_FETCH_REQUEST_BODY_OK);
    }
    candidate->cfg = cfg;

    const char* method = configured_method(cfg);
    candidate->request = bx_fetch_request_new_prepared(method, target);
    if (!candidate->request) {
        return prepare_failure(candidate, error, BX_FETCH_PREPARE_FAILURE_REQUEST, errno ? errno : ENOMEM, protocol_decision, BX_FETCH_REQUEST_BODY_OK);
    }

    BxFetchRequestBodyResult body_result = apply_request_body(cfg, candidate->request);
    if (body_result != BX_FETCH_REQUEST_BODY_OK) {
        return prepare_failure(candidate, error, BX_FETCH_PREPARE_FAILURE_REQUEST_BODY, errno ? errno : EIO, protocol_decision, body_result);
    }

    bool output_policy_allowed = !cfg->download.spider && method_allows_existing_output_policy(candidate->request->method);
    BxFetchWriterMode writer_mode = output_policy_allowed && cfg->download.continue_download ? WRITER_RESUME : WRITER_CREATE;
    const char* writer_path = cfg->download.spider ? "-" : output_path;
    int backups = cfg->download.spider ? 0 : cfg->recursive.backups;
    candidate->writer = bx_fetch_writer_open_with_options(writer_path, writer_mode, backups, cfg->download.unlink);
    if (!candidate->writer) {
        return prepare_failure(candidate, error, BX_FETCH_PREPARE_FAILURE_WRITER, errno ? errno : EIO, protocol_decision, body_result);
    }
    if (cfg->download.no_clobber && !cfg->download.spider &&
        bx_fetch_writer_set_final_path_exclusive(candidate->writer, writer_path) != 0) {
        return prepare_failure(candidate, error, BX_FETCH_PREPARE_FAILURE_WRITER, errno ? errno : EIO, protocol_decision, body_result);
    }

    if (writer_mode == WRITER_RESUME && add_resume_header(candidate->request, candidate->writer) != 0) {
        return prepare_failure(candidate, error, BX_FETCH_PREPARE_FAILURE_REQUEST_HEADER, errno ? errno : EINVAL, protocol_decision, body_result);
    }

    ConditionalHeadersResult conditional_result = CONDITIONAL_HEADERS_OK;
    if (output_policy_allowed && cfg->download.timestamping) {
        conditional_result = add_conditional_headers(cfg, candidate->writer, candidate->request);
    }
    if (conditional_result != CONDITIONAL_HEADERS_OK) {
        int error_number = errno ? errno : EIO;
        return prepare_failure(candidate, error, conditional_result == CONDITIONAL_HEADERS_REQUEST_FAILURE ? BX_FETCH_PREPARE_FAILURE_REQUEST_HEADER : BX_FETCH_PREPARE_FAILURE_METADATA, error_number,
                               protocol_decision, body_result);
    }

    return candidate;
}

const BxFetchRequest* bx_fetch_transfer_candidate_request(const BxFetchTransferCandidate* candidate) {
    return candidate ? candidate->request : NULL;
}

void bx_fetch_transfer_candidate_abort(BxFetchTransferCandidate* candidate) {
    if (!candidate)
        return;
    if (candidate->writer)
        bx_fetch_writer_abort(candidate->writer);
    bx_fetch_request_free(candidate->request);
    free(candidate);
}

static int candidate_headers_callback(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer) {
    CandidateCallbacks* callbacks = userdata;
    if (!callbacks || !callbacks->cfg) {
        errno = EINVAL;
        return -1;
    }
    if (response->status_code == 304)
        return bx_fetch_transfer_stage_not_modified(callbacks->cfg, request, response, writer);
    if (callbacks->headers_callback && callbacks->headers_callback(callbacks->userdata, request, response, writer) != 0)
        return -1;
    const char* final_path = bx_fetch_writer_get_path(writer);
    char* final_path_copy = final_path ? strdup(final_path) : NULL;
    if (!final_path_copy)
        return -1;
    free(callbacks->output_path);
    callbacks->output_path = final_path_copy;
    return bx_fetch_transfer_stage_response(callbacks->cfg, request, response, writer);
}

static void candidate_completion_callback(void* userdata, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchError result) {
    CandidateCallbacks* callbacks = userdata;
    if (!callbacks)
        return;

    BxFetchTransferCompletion completion = {
        .request = request,
        .response = response,
        .output_path = callbacks->output_path,
        .result = result,
        .retryable_hint = bx_fetch_transfer_retryable_hint(callbacks->cfg, response, result),
    };
    if (callbacks->completion_callback)
        callbacks->completion_callback(callbacks->userdata, &completion);
    free(callbacks->output_path);
    free(callbacks);
}

int bx_fetch_transfer_candidate_submit(BxFetchTransferCandidate* candidate,
                                       BxFetchEngine* engine,
                                       BxFetchTransferHeadersCallback headers_cb,
                                       BxFetchTransferCompletionCallback callback,
                                       void* userdata,
                                       BxFetchRedirectPolicyCallback redirect_cb,
                                       void* redirect_userdata,
                                       BxFetchNetSetupError* setup_error) {
    if (!candidate || !engine || !candidate->request || !candidate->writer) {
        if (candidate)
            bx_fetch_transfer_candidate_abort(candidate);
        errno = EINVAL;
        return -1;
    }

    CandidateCallbacks* callbacks = calloc(1, sizeof(*callbacks));
    if (!callbacks) {
        bx_fetch_transfer_candidate_abort(candidate);
        return -1;
    }
    callbacks->cfg = candidate->cfg;
    callbacks->headers_callback = headers_cb;
    callbacks->completion_callback = callback;
    callbacks->userdata = userdata;
    const char* output_path = bx_fetch_writer_get_path(candidate->writer);
    callbacks->output_path = output_path ? strdup(output_path) : NULL;
    if (!callbacks->output_path) {
        free(callbacks);
        bx_fetch_transfer_candidate_abort(candidate);
        return -1;
    }

    int result = bx_fetch_engine_submit_with_setup_error(engine, candidate->request, candidate->writer, candidate_headers_callback, candidate_completion_callback, callbacks, redirect_cb,
                                                         redirect_userdata, setup_error);
    if (result == 0) {
        candidate->request = NULL;
        candidate->writer = NULL;
    }
    else {
        free(callbacks->output_path);
        free(callbacks);
    }
    bx_fetch_transfer_candidate_abort(candidate);
    return result;
}
