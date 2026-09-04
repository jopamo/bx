#ifndef BX_FETCH_DOCUMENT_H
#define BX_FETCH_DOCUMENT_H

/* BX_FETCH_HEADER_OWNER: crawl */
/* BX_FETCH_HEADER_CONSUMERS: crawl, core, applet */

/*
 * Cold-path processing for committed downloaded documents. The processor
 * opens through the fetch secure-path authority, reads one descriptor once,
 * classifies from that same bounded buffer, and delegates lexical parsing to
 * the shared HTML/CSS parser.
 */

#include "html.h"
#include "url.h"
#include <stddef.h>

#ifndef BX_FETCH_DOCUMENT_MAX_BYTES
#define BX_FETCH_DOCUMENT_MAX_BYTES BX_FETCH_DOCUMENT_PARSE_MAX_BYTES
#endif

typedef enum {
    BX_FETCH_DOCUMENT_NONE = 0,
    BX_FETCH_DOCUMENT_HTML,
    BX_FETCH_DOCUMENT_CSS,
} BxFetchDocumentKind;

typedef enum {
    BX_FETCH_DOCUMENT_FAILURE_NONE = 0,
    BX_FETCH_DOCUMENT_FAILURE_INVALID_ARGUMENT,
    BX_FETCH_DOCUMENT_FAILURE_OPEN,
    BX_FETCH_DOCUMENT_FAILURE_READ,
    BX_FETCH_DOCUMENT_FAILURE_ALLOCATION,
    BX_FETCH_DOCUMENT_FAILURE_TOO_LARGE,
    BX_FETCH_DOCUMENT_FAILURE_PARSE,
    BX_FETCH_DOCUMENT_FAILURE_CALLBACK,
} BxFetchDocumentFailure;

typedef struct {
    BxFetchDocumentKind kind;
    BxFetchDocumentFailure failure;
    int error_number;
} BxFetchDocumentOutcome;

/*
 * reference is borrowed parser output. Return 0 to continue or nonzero to
 * abort extraction; set errno to preserve a specific callback failure.
 */
typedef int (*BxFetchDocumentLinkFn)(void* userdata, const char* reference, BxFetchHtmlLinkKind kind);

/*
 * Returns 0 for a parsed document or an intentionally skipped non-document,
 * and -1 with a typed outcome for open/read/limit/parser/callback failure.
 * base is already prepared and is never reparsed.
 */
int bx_fetch_document_extract_links(const char* path, const char* content_type, const BxFetchPreparedUrl* base, BxFetchDocumentLinkFn callback, void* userdata, BxFetchDocumentOutcome* outcome);

#endif  // BX_FETCH_DOCUMENT_H
