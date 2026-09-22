#ifndef BX_FETCH_HTML_SITE_MARKDOWN_H
#define BX_FETCH_HTML_SITE_MARKDOWN_H

#include <stdbool.h>

#if HAVE_LEXBOR
#include <lexbor/dom/interfaces/node.h>

typedef enum {
    BX_FETCH_MARKDOWN_SITE_GENERIC = 0,
    BX_FETCH_MARKDOWN_SITE_KERNEL_SPHINX,
    BX_FETCH_MARKDOWN_SITE_MAN7,
    BX_FETCH_MARKDOWN_SITE_RFC_EDITOR,
    BX_FETCH_MARKDOWN_SITE_WIKIPEDIA,
} BxFetchMarkdownSite;

BxFetchMarkdownSite bx_fetch_markdown_site_for_url(const char* base_url);
bool bx_fetch_site_markdown_skip_node(BxFetchMarkdownSite site, lxb_dom_node_t* node);
bool bx_fetch_site_markdown_unwrap_node(BxFetchMarkdownSite site, lxb_dom_node_t* node);
#endif

#endif
