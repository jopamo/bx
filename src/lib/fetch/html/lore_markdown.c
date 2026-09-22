#include "lib/fetch/html/lore_markdown.h"

#if HAVE_LEXBOR
/*
 * public-inbox intentionally uses preformatted siblings instead of semantic
 * page regions. Keep its stable reply and mirror boilerplate rules out of the
 * generic HTML mapping.
 */
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>

bool bx_fetch_lore_markdown_url_matches(const char* base_url) {
    if (!base_url)
        return false;
    const char* authority = strstr(base_url, "://");
    if (!authority)
        return false;
    authority += 3;
    static const char host[] = "lore.kernel.org";
    size_t host_length = sizeof(host) - 1u;
    return strncasecmp(authority, host, host_length) == 0 && (authority[host_length] == '\0' || authority[host_length] == '/' || authority[host_length] == '?' || authority[host_length] == '#');
}

static const lxb_char_t* lore_attribute(lxb_dom_element_t* element, const char* name, size_t* length) {
    return lxb_dom_element_get_attribute(element, (const lxb_char_t*)name, strlen(name), length);
}

static bool lore_text_starts_with(const lxb_char_t* text, size_t length, const char* prefix) {
    if (!text)
        return false;
    size_t start = 0;
    while (start < length && isspace(text[start]))
        start++;
    size_t prefix_length = strlen(prefix);
    return length - start >= prefix_length && memcmp(text + start, prefix, prefix_length) == 0;
}

static bool lore_pre_is_junk(lxb_dom_node_t* node) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_PRE) {
        return false;
    }
    size_t id_length = 0;
    const lxb_char_t* id = lore_attribute(lxb_dom_interface_element(node), "id", &id_length);
    if (id && id_length == 1u && id[0] == 'R')
        return true;

    size_t length = 0;
    lxb_char_t* text = lxb_dom_node_text_content(node, &length);
    bool skip = lore_text_starts_with(text, length, "This is a public inbox, see");
    if (text)
        lxb_dom_document_destroy_text(node->owner_document, text);
    return skip;
}

static bool lore_follows_reply_instructions(lxb_dom_node_t* node) {
    for (lxb_dom_node_t* previous = node->prev; previous; previous = previous->prev) {
        if (previous->type != LXB_DOM_NODE_TYPE_ELEMENT)
            continue;
        if (previous->local_name == LXB_TAG_HR)
            return false;
        size_t id_length = 0;
        const lxb_char_t* id = lore_attribute(lxb_dom_interface_element(previous), "id", &id_length);
        if (previous->local_name == LXB_TAG_PRE && id && id_length == 1u && id[0] == 'R') {
            return true;
        }
    }
    return false;
}

bool bx_fetch_lore_markdown_skip_node(lxb_dom_node_t* node) {
    if (!node)
        return false;
    if (lore_pre_is_junk(node))
        return true;
    if (lore_follows_reply_instructions(node))
        return true;
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t* text = lxb_dom_interface_character_data(node);
        return lore_text_starts_with(text->data.data, text->data.length, "Be sure your reply has a Subject: header");
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT || node->local_name != LXB_TAG_HR) {
        return false;
    }
    for (lxb_dom_node_t* next = node->next; next; next = next->next) {
        if (next->type == LXB_DOM_NODE_TYPE_TEXT) {
            lxb_dom_character_data_t* text = lxb_dom_interface_character_data(next);
            bool whitespace = true;
            for (size_t index = 0; index < text->data.length; index++) {
                if (!isspace(text->data.data[index])) {
                    whitespace = false;
                    break;
                }
            }
            if (whitespace)
                continue;
        }
        return lore_pre_is_junk(next);
    }
    return false;
}

#else
typedef int BxFetchLoreMarkdownDisabled;
#endif
