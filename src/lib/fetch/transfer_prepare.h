#ifndef BX_FETCH_TRANSFER_PREPARE_H
#define BX_FETCH_TRANSFER_PREPARE_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core */

/*
 * A transfer candidate is private, fully prepared request/writer state.
 * Preparation normalizes no URLs: callers supply an existing prepared target.
 * Submission is the single ownership commit point. It consumes the candidate;
 * failure aborts only candidate-owned state, while success transfers request
 * and writer ownership to the transport. The configuration is borrowed and
 * must outlive the candidate and every successfully submitted transfer.
 */

#include "config.h"
#include "net.h"
#include "transfer_completion.h"

typedef struct BxFetchTransferCandidate BxFetchTransferCandidate;

typedef enum {
    BX_FETCH_PREPARE_FAILURE_NONE = 0,
    BX_FETCH_PREPARE_FAILURE_INVALID_ARGUMENT,
    BX_FETCH_PREPARE_FAILURE_PROTOCOL_POLICY,
    BX_FETCH_PREPARE_FAILURE_REQUEST,
    BX_FETCH_PREPARE_FAILURE_REQUEST_BODY,
    BX_FETCH_PREPARE_FAILURE_REQUEST_HEADER,
    BX_FETCH_PREPARE_FAILURE_METADATA,
    BX_FETCH_PREPARE_FAILURE_WRITER,
} BxFetchPrepareFailureKind;

typedef struct {
    BxFetchPrepareFailureKind kind;
    int error_number;
    BxFetchProtocolDecision protocol_decision;
    BxFetchRequestBodyResult body_result;
} BxFetchPrepareError;

BxFetchTransferCandidate* bx_fetch_transfer_candidate_prepare(const struct bx_fetch_config* cfg, const BxFetchPreparedUrl* target, const char* output_path, BxFetchPrepareError* error);

/* Borrowed request view, valid until abort/submission consumes the candidate. */
const BxFetchRequest* bx_fetch_transfer_candidate_request(const BxFetchTransferCandidate* candidate);

/* Aborts candidate output and releases request/body resources. */
void bx_fetch_transfer_candidate_abort(BxFetchTransferCandidate* candidate);

/*
 * Consumes candidate on every return path. Success transfers request/writer
 * ownership to engine. Submission failure aborts the private writer.
 */
int bx_fetch_transfer_candidate_submit(BxFetchTransferCandidate* candidate,
                                       BxFetchEngine* engine,
                                       BxFetchTransferHeadersCallback headers_cb,
                                       BxFetchTransferCompletionCallback callback,
                                       void* userdata,
                                       BxFetchRedirectPolicyCallback redirect_cb,
                                       void* redirect_userdata,
                                       BxFetchNetSetupError* setup_error);

#endif  // BX_FETCH_TRANSFER_PREPARE_H
