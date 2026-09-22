#ifndef BX_FETCH_HTML_LORE_MARKDOWN_H
#define BX_FETCH_HTML_LORE_MARKDOWN_H

#include <stdbool.h>

#if HAVE_LEXBOR
#include <lexbor/dom/interfaces/node.h>

bool bx_fetch_lore_markdown_url_matches(const char* base_url);
bool bx_fetch_lore_markdown_skip_node(lxb_dom_node_t* node);
#endif

#endif
