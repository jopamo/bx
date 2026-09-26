#ifndef BX_FETCH_HTML_LORE_MARKDOWN_H
#define BX_FETCH_HTML_LORE_MARKDOWN_H

#include <stdbool.h>

#if HAVE_LEXBOR || HAVE_NATIVE_HTML
#if HAVE_NATIVE_HTML
#include <liblexa/dom/node_store.h>
#else
#include <lexbor/dom/interfaces/node.h>
#endif

bool bx_fetch_lore_markdown_url_matches(const char* base_url);
#if HAVE_NATIVE_HTML
lxa_status_t bx_fetch_lore_markdown_skip_node(lxa_dom_nodes_t* nodes, lxa_dom_ref_t node, bool* skip);
#else
bool bx_fetch_lore_markdown_skip_node(lxb_dom_node_t* node);
#endif
#endif

#endif
