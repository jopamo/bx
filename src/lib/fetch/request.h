#ifndef BX_FETCH_REQUEST_H
#define BX_FETCH_REQUEST_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, core, net */

/*
 * Layering contract:
 * - Runtime request model shared between core planning and net execution.
 * - Callers configure requests here instead of mutating libcurl handles directly.
 *
 * Ownership and lifetime:
 * - bx_fetch_request_new() returns a heap-owned BxFetchRequest released by bx_fetch_request_free().
 * - Every request owns immutable prepared URL state. Raw URL construction
 *   normalizes once; prepared construction clones without reparsing.
 * - The transport identity may contain authority userinfo. Observable code
 *   must use bx_fetch_request_url_for_display().
 * - bx_fetch_request_add_header() validates and copies normalized name/value strings.
 * - `body` is owned by BxFetchRequest and freed by bx_fetch_request_free().
 * - bx_fetch_request_set_body_file() opens and validates one regular-file upload source;
 *   BxFetchRequest owns that descriptor until bx_fetch_request_free().
 */

#include "types.h"
#include "url.h"

typedef struct BxFetchRequestBodyFile BxFetchRequestBodyFile;

typedef enum {
    BX_FETCH_REQUEST_BODY_OK = 0,
    BX_FETCH_REQUEST_BODY_IO,
    BX_FETCH_REQUEST_BODY_POLICY,
    BX_FETCH_REQUEST_BODY_MEMORY,
} BxFetchRequestBodyResult;

typedef struct {
    char* method;
    BxFetchPreparedUrl* target;
    BxFetchHeader* headers;
    size_t header_count;
    size_t header_capacity;

    void* body;
    size_t body_len;
    BxFetchRequestBodyFile* body_file;
} BxFetchRequest;

BxFetchRequest* bx_fetch_request_new(const char* method, const char* url);
BxFetchRequest* bx_fetch_request_new_canonical(const char* method, const char* canonical_url);
BxFetchRequest* bx_fetch_request_new_prepared(const char* method, const BxFetchPreparedUrl* target);
void bx_fetch_request_free(BxFetchRequest* req);
const BxFetchPreparedUrl* bx_fetch_request_target(const BxFetchRequest* req);
const char* bx_fetch_request_url_for_transport(const BxFetchRequest* req);
/* Returns userinfo-free text, never the credential-bearing transport URL. */
const char* bx_fetch_request_url_for_display(const BxFetchRequest* req);
/*
 * Validates and copies `name`/`value` into request-owned storage.
 * Returns -1 with errno EINVAL for policy/grammar errors or ENOMEM for
 * allocation failure.
 */
int bx_fetch_request_add_header(BxFetchRequest* req, const char* name, const char* value);

/*
 * Configure a fixed-length streaming request body from `path`.
 *
 * Only regular files are accepted. The path is opened once and is never
 * reopened for redirects; reads and rewinds operate on the same descriptor.
 * A successful call replaces any existing memory or file body.
 */
BxFetchRequestBodyResult bx_fetch_request_set_body_file(BxFetchRequest* req, const char* path);
bool bx_fetch_request_has_body_file(const BxFetchRequest* req);
uint64_t bx_fetch_request_body_file_size(const BxFetchRequest* req);
/*
 * Reads at most `capacity` bytes from the fixed-length opened source.
 * Premature EOF is an I/O failure rather than a successful short body.
 */
int bx_fetch_request_body_file_read(BxFetchRequest* req, void* buffer, size_t capacity, size_t* read_out);
/*
 * Repositions the body stream for libcurl redirect/retry handling.
 * The resulting offset must remain within the opened file's original length.
 */
int bx_fetch_request_body_file_seek(BxFetchRequest* req, int64_t offset, int origin);

#endif  // BX_FETCH_REQUEST_H
