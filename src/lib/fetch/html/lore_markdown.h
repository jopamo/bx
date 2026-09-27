#ifndef BX_FETCH_HTML_LORE_MARKDOWN_H
#define BX_FETCH_HTML_LORE_MARKDOWN_H

#include <stdbool.h>

#include <liblexa/dom/node_store.h>

bool bx_fetch_lore_markdown_url_matches(const char* base_url);
lxa_status_t bx_fetch_lore_markdown_skip_node(lxa_dom_nodes_t* nodes, lxa_dom_ref_t node, bool* skip);
#endif
