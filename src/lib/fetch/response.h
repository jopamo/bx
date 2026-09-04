#ifndef MIRA_RESPONSE_H
#define MIRA_RESPONSE_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: runtime, core, net */

/*
 * Layering contract:
 * - Runtime response model is the only payload/header/status carrier shared
 *   from net to core.
 *
 * Ownership and lifetime:
 * - response_new() returns a heap-owned Response released by response_free().
 * - response_add_header() copies name/value strings into response-owned storage.
 * - Response pointers passed via callbacks are borrowed and invalid after their
 *   owning transfer callback returns.
 */

#include "error.h"
#include "types.h"

#define MIRA_RESPONSE_HEADER_LINE_LIMIT_TEXT "64 KiB"
#define MIRA_RESPONSE_HEADER_LINE_MAX_BYTES ((size_t)64 * 1024u)
#define MIRA_RESPONSE_HEADER_BLOCK_LIMIT_TEXT "256 KiB"
#define MIRA_RESPONSE_HEADER_BLOCK_MAX_BYTES ((size_t)256 * 1024u)
#define MIRA_RESPONSE_HEADER_FIELD_LIMIT_TEXT "1024"
#define MIRA_RESPONSE_HEADER_MAX_FIELDS ((size_t)1024)

typedef enum {
    MIRA_RESPONSE_HEADER_POLICY_OK = 0,
    MIRA_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE,
    MIRA_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE,
    MIRA_RESPONSE_HEADER_POLICY_TOO_MANY_FIELDS,
} MiraResponseHeaderPolicyFailure;

typedef struct {
    int status_code;
    char *effective_url;

    MiraHeader *headers;
    size_t header_count;
    size_t header_capacity;

    // Total size of body if known (Content-Length)
    int64_t content_length;

    char *content_type;

    // Error code if transfer failed
    int error_code;
    int error_number;
    MiraTransportErrorKind transport_error_kind;
    char *transport_error_detail;
    bool request_body_io_failed;
    size_t header_bytes;
    MiraResponseHeaderPolicyFailure header_policy_failure;
} Response;

Response *response_new(void);
void response_free(Response *resp);
/*
 * Copies `name`/`value` into bounded response-owned storage.
 * Returns -1 with errno EFBIG for line/block/field limits, ENOMEM for
 * allocation failure, or EINVAL for invalid arguments.
 */
int response_add_header(Response *resp, const char *name, const char *value);
const char *mira_response_header_policy_failure_summary(
    MiraResponseHeaderPolicyFailure failure);

#endif // MIRA_RESPONSE_H
