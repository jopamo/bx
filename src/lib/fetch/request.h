#ifndef MIRA_REQUEST_H
#define MIRA_REQUEST_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: runtime, core, net */

/*
 * Layering contract:
 * - Runtime request model shared between core planning and net execution.
 * - Callers configure requests here instead of mutating libcurl handles directly.
 *
 * Ownership and lifetime:
 * - request_new() returns a heap-owned Request released by request_free().
 * - request_new_canonical() is the internal fast path for URLs already
 *   canonicalized at a trust boundary.
 * - `url` is the canonical transport identity after net submission and may
 *   contain authority userinfo. `display_url` is the separately owned,
 *   userinfo-free text for all observable output.
 * - request_add_header() validates and copies normalized name/value strings.
 * - `body` is owned by Request and freed by request_free().
 * - request_set_body_file() opens and validates one regular-file upload source;
 *   Request owns that descriptor until request_free().
 */

#include "types.h"

typedef struct MiraRequestBodyFile MiraRequestBodyFile;

typedef enum {
    MIRA_REQUEST_BODY_OK = 0,
    MIRA_REQUEST_BODY_IO,
    MIRA_REQUEST_BODY_POLICY,
    MIRA_REQUEST_BODY_MEMORY,
} MiraRequestBodyResult;

typedef struct {
    char *method;
    char *url;
    char *display_url;
    bool url_is_canonical;
    MiraHeader *headers;
    size_t header_count;
    size_t header_capacity;

    void *body;
    size_t body_len;
    MiraRequestBodyFile *body_file;
} Request;

Request *request_new(const char *method, const char *url);
Request *request_new_canonical(const char *method, const char *canonical_url);
void request_free(Request *req);
/* Returns userinfo-free text or a fail-closed placeholder, never `req->url`. */
const char *request_url_for_display(const Request *req);
/*
 * Validates and copies `name`/`value` into request-owned storage.
 * Returns -1 with errno EINVAL for policy/grammar errors or ENOMEM for
 * allocation failure.
 */
int request_add_header(Request *req, const char *name, const char *value);

/*
 * Configure a fixed-length streaming request body from `path`.
 *
 * Only regular files are accepted. The path is opened once and is never
 * reopened for redirects; reads and rewinds operate on the same descriptor.
 * A successful call replaces any existing memory or file body.
 */
MiraRequestBodyResult request_set_body_file(Request *req, const char *path);
bool request_has_body_file(const Request *req);
uint64_t request_body_file_size(const Request *req);
/*
 * Reads at most `capacity` bytes from the fixed-length opened source.
 * Premature EOF is an I/O failure rather than a successful short body.
 */
int request_body_file_read(Request *req, void *buffer, size_t capacity,
                           size_t *read_out);
/*
 * Repositions the body stream for libcurl redirect/retry handling.
 * The resulting offset must remain within the opened file's original length.
 */
int request_body_file_seek(Request *req, int64_t offset, int origin);

#endif // MIRA_REQUEST_H
