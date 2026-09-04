#ifndef BX_FETCH_HTML_H
#define BX_FETCH_HTML_H

/* BX_FETCH_HEADER_OWNER: crawl */
/* BX_FETCH_HEADER_CONSUMERS: crawl, core */

/*
 * Layering contract:
 * - HTML/CSS link extraction and rewrite stays in crawl parsing code.
 * - Core provides policy decisions via callbacks and does not parse markup
 *   directly.
 *
 * Ownership and lifetime:
 * - Callback URL arguments are borrowed and only valid during the callback.
 * - BxFetchLinkRewriteCallback must return a heap string on rewrite; ownership of
 *   the returned string is transferred to the parser.
 * - bx_fetch_html_convert_links() returns a heap buffer owned by the caller.
 */

#include <stddef.h>

/*
 * Parsing requires a complete document and never falls back to a prefix.
 * Callers must reject larger inputs before allocating a parser buffer; parser
 * entry points enforce the same boundary for direct API use.
 */
#ifndef BX_FETCH_DOCUMENT_PARSE_LIMIT_TEXT
#define BX_FETCH_DOCUMENT_PARSE_LIMIT_TEXT "16 MiB"
#endif
#ifndef BX_FETCH_DOCUMENT_PARSE_MAX_BYTES
#define BX_FETCH_DOCUMENT_PARSE_MAX_BYTES ((size_t)16 * 1024u * 1024u)
#endif

/* `url` is transient; copy it if it must outlive the callback. */
typedef void (*BxFetchLinkCallback)(void* userdata, const char* url);

typedef enum {
    BX_FETCH_HTML_LINK_NAVIGATION = 0,
    BX_FETCH_HTML_LINK_REQUISITE,
} BxFetchHtmlLinkKind;

/* `url` is transient; copy it if it must outlive the callback. */
typedef void (*BxFetchHtmlLinkCallback)(void* userdata, const char* url, BxFetchHtmlLinkKind kind);

/*
 * `url` is transient input.
 * Return NULL to keep the original URL, or a heap-allocated replacement string.
 */
typedef char* (*BxFetchLinkRewriteCallback)(void* userdata, const char* url);

/* `base_url` is reserved for API compatibility; current extraction is lexical. */
int bx_fetch_html_extract_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkCallback cb, void* userdata);
/* Typed extraction distinguishes navigation links from embedded requisites. */
int bx_fetch_html_extract_links_typed(const char* base_url, const char* html_data, size_t len, BxFetchHtmlLinkCallback cb, void* userdata);
/* Returned document is heap-allocated and must be freed by the caller. */
char* bx_fetch_html_convert_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkRewriteCallback cb, void* userdata);
/* `base_url` is reserved for API compatibility; current extraction is lexical. */
int bx_fetch_css_extract_links(const char* base_url, const char* css_data, size_t len, BxFetchLinkCallback cb, void* userdata);

#endif  // BX_FETCH_HTML_H
