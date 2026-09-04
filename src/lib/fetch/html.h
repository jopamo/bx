#ifndef MIRA_HTML_H
#define MIRA_HTML_H

/* MIRA_HEADER_OWNER: crawl */
/* MIRA_HEADER_CONSUMERS: crawl, core */

/*
 * Layering contract:
 * - HTML/CSS link extraction and rewrite stays in crawl parsing code.
 * - Core provides policy decisions via callbacks and does not parse markup
 *   directly.
 *
 * Ownership and lifetime:
 * - Callback URL arguments are borrowed and only valid during the callback.
 * - MiraLinkRewriteCallback must return a heap string on rewrite; ownership of
 *   the returned string is transferred to the parser.
 * - html_convert_links() returns a heap buffer owned by the caller.
 */

#include <stddef.h>

/*
 * Parsing requires a complete document and never falls back to a prefix.
 * Callers must reject larger inputs before allocating a parser buffer; parser
 * entry points enforce the same boundary for direct API use.
 */
#define MIRA_DOCUMENT_PARSE_LIMIT_TEXT "16 MiB"
#define MIRA_DOCUMENT_PARSE_MAX_BYTES ((size_t)16 * 1024u * 1024u)

/* `url` is transient; copy it if it must outlive the callback. */
typedef void (*MiraLinkCallback)(void* userdata, const char* url);

typedef enum {
    MIRA_HTML_LINK_NAVIGATION = 0,
    MIRA_HTML_LINK_REQUISITE,
} MiraHtmlLinkKind;

/* `url` is transient; copy it if it must outlive the callback. */
typedef void (*MiraHtmlLinkCallback)(void* userdata, const char* url, MiraHtmlLinkKind kind);

/*
 * `url` is transient input.
 * Return NULL to keep the original URL, or a heap-allocated replacement string.
 */
typedef char* (*MiraLinkRewriteCallback)(void* userdata, const char* url);

/* `base_url` is reserved for API compatibility; current extraction is lexical. */
int html_extract_links(const char* base_url, const char* html_data, size_t len, MiraLinkCallback cb, void* userdata);
/* Typed extraction distinguishes navigation links from embedded requisites. */
int html_extract_links_typed(const char* base_url, const char* html_data, size_t len, MiraHtmlLinkCallback cb, void* userdata);
/* Returned document is heap-allocated and must be freed by the caller. */
char* html_convert_links(const char* base_url, const char* html_data, size_t len, MiraLinkRewriteCallback cb, void* userdata);
/* `base_url` is reserved for API compatibility; current extraction is lexical. */
int css_extract_links(const char* base_url, const char* css_data, size_t len, MiraLinkCallback cb, void* userdata);

#endif  // MIRA_HTML_H
