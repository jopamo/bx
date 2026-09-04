#ifndef BX_FETCH_TRANSFER_COMPLETION_H
#define BX_FETCH_TRANSFER_COMPLETION_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core, applet */

/*
 * Completion is the typed boundary between shared transfer orchestration and
 * applet policy. The request/response pointers are borrowed for the callback.
 * output_state records what happened to the candidate writer independently of
 * protocol success, so callers do not infer publication from HTTP status.
 */

#include "config.h"
#include "request.h"
#include "response.h"
#include "writer.h"

typedef struct {
    const BxFetchRequest* request;
    const BxFetchResponse* response;
    const char* output_path;
    BxFetchError result;
    bool retryable_hint;
} BxFetchTransferCompletion;

typedef void (*BxFetchTransferCompletionCallback)(void* userdata, const BxFetchTransferCompletion* completion);

/*
 * Applies shared response mechanics to the still-private writer after applet
 * header policy has selected its final path. Payload, sidecar metadata, mtime,
 * and xattrs are then committed by one writer close.
 */
int bx_fetch_transfer_stage_response(const struct bx_fetch_config* cfg, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer);
/*
 * Stages a metadata-only 304 refresh while preserving payload identity.
 * The authoritative xattr policy is fail-closed: requesting xattrs returns
 * ENOTSUP before mutation because inode xattrs cannot commit atomically with
 * the sidecar exchange, and replacing the payload inode is incompatible.
 */
int bx_fetch_transfer_stage_not_modified(const struct bx_fetch_config* cfg, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer);

/* Policy-light retryability hint; the scheduler still owns attempts/delays. */
bool bx_fetch_transfer_retryable_hint(const struct bx_fetch_config* cfg, const BxFetchResponse* response, BxFetchError result);

#endif  // BX_FETCH_TRANSFER_COMPLETION_H
