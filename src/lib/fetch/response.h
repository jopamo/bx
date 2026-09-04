#ifndef BX_FETCH_RESPONSE_H
#define BX_FETCH_RESPONSE_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, core, net */

/*
 * Layering contract:
 * - Runtime response model is the only payload/header/status carrier shared
 *   from net to core.
 *
 * Ownership and lifetime:
 * - bx_fetch_response_new() returns a heap-owned BxFetchResponse released by bx_fetch_response_free().
 * - bx_fetch_response_add_header() copies name/value strings into response-owned storage.
 * - BxFetchResponse pointers passed via callbacks are borrowed and invalid after their
 *   owning transfer callback returns.
 */

#include "error.h"
#include "types.h"

#define BX_FETCH_RESPONSE_HEADER_LINE_LIMIT_TEXT "64 KiB"
#define BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES ((size_t)64 * 1024u)
#define BX_FETCH_RESPONSE_HEADER_BLOCK_LIMIT_TEXT "256 KiB"
#define BX_FETCH_RESPONSE_HEADER_BLOCK_MAX_BYTES ((size_t)256 * 1024u)
#define BX_FETCH_RESPONSE_HEADER_FIELD_LIMIT_TEXT "1024"
#define BX_FETCH_RESPONSE_HEADER_MAX_FIELDS ((size_t)1024)

typedef enum {
    BX_FETCH_RESPONSE_HEADER_POLICY_OK = 0,
    BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE,
    BX_FETCH_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE,
    BX_FETCH_RESPONSE_HEADER_POLICY_TOO_MANY_FIELDS,
} BxFetchResponseHeaderPolicyFailure;

typedef struct {
    int status_code;
    char* effective_url;

    BxFetchHeader* headers;
    size_t header_count;
    size_t header_capacity;

    // Total size of body if known (Content-Length)
    int64_t content_length;

    char* content_type;

    // Error code if transfer failed
    int error_code;
    int error_number;
    BxFetchTransportErrorKind transport_error_kind;
    char* transport_error_detail;
    bool request_body_io_failed;
    size_t header_bytes;
    BxFetchResponseHeaderPolicyFailure header_policy_failure;
} BxFetchResponse;

BxFetchResponse* bx_fetch_response_new(void);
void bx_fetch_response_free(BxFetchResponse* resp);
/*
 * Copies `name`/`value` into bounded response-owned storage.
 * Returns -1 with errno EFBIG for line/block/field limits, ENOMEM for
 * allocation failure, or EINVAL for invalid arguments.
 */
int bx_fetch_response_add_header(BxFetchResponse* resp, const char* name, const char* value);
const char* bx_fetch_response_header_policy_failure_summary(BxFetchResponseHeaderPolicyFailure failure);

#endif  // BX_FETCH_RESPONSE_H
