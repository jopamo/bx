#include "lib/fetch/html/lore_markdown.h"

/*
 * public-inbox intentionally uses preformatted siblings instead of semantic
 * page regions. Keep its stable reply and mirror boilerplate rules out of the
 * generic HTML mapping.
 */
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include "lib/fetch/html.h"

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

static bool lore_text_starts_with(lxa_span_t text, const char* prefix) {
    size_t start = 0;
    while (start < text.length && isspace((unsigned char)text.data[start]))
        start++;
    size_t length = strlen(prefix);
    return text.length - start >= length && memcmp(text.data + start, prefix, length) == 0;
}

static lxa_status_t lore_attribute(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                   const char* name, lxa_span_t* value) {
    lxa_dom_ref_t attribute;
    lxa_span_t key, lookup = {(const uint8_t*)name, strlen(name)};
    *value = (lxa_span_t){0};
    lxa_status_t status = lxa_dom_nodes_find_attribute(nodes, element, lookup, &attribute);
    if (status != LXA_OK || !attribute.handle)
        return status;
    return lxa_dom_nodes_attribute_read(nodes, attribute, &key, value);
}

static lxa_status_t lore_is_tag(lxa_dom_nodes_t* nodes, lxa_dom_ref_t node,
                                const char* tag, bool* matches) {
    lxa_dom_record_t record;
    lxa_status_t status = lxa_dom_nodes_read(nodes, node, &record);
    if (status != LXA_OK)
        return status;
    *matches = false;
    if (record.kind != LXA_DOM_KIND_ELEMENT)
        return LXA_OK;
    lxa_span_t name;
    status = lxa_dom_nodes_element_name(nodes, node, &name);
    if (status == LXA_OK)
        *matches = name.length == strlen(tag) && memcmp(name.data, tag, name.length) == 0;
    return status;
}

static lxa_status_t lore_pre_is_junk(lxa_dom_nodes_t* nodes, lxa_dom_ref_t node,
                                     bool* skip) {
    lxa_status_t status = lore_is_tag(nodes, node, "pre", skip);
    if (status != LXA_OK || !*skip)
        return status;
    lxa_span_t id;
    status = lore_attribute(nodes, node, "id", &id);
    if (status != LXA_OK)
        return status;
    if (id.length == 1 && id.data[0] == 'R')
        return LXA_OK;
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_buffer_t text;
    status = lxa_buffer_init(&text, &allocator);
    if (status != LXA_OK)
        return status;
    status = lxa_dom_nodes_text_content(nodes, node, &text, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    *skip = status == LXA_OK && lore_text_starts_with((lxa_span_t){text.data, text.length},
                                                      "This is a public inbox, see");
    lxa_buffer_destroy(&text);
    return status;
}

static lxa_status_t lore_follows_reply_instructions(lxa_dom_nodes_t* nodes,
                                                     lxa_dom_ref_t node, bool* skip) {
    lxa_status_t status = lxa_dom_nodes_prev_sibling(nodes, node, &node);
    *skip = false;
    while (status == LXA_OK && node.handle) {
        lxa_dom_record_t record;
        status = lxa_dom_nodes_read(nodes, node, &record);
        if (status != LXA_OK)
            return status;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            bool match;
            status = lore_is_tag(nodes, node, "hr", &match);
            if (status != LXA_OK || match)
                return status;
            status = lore_is_tag(nodes, node, "pre", &match);
            if (status != LXA_OK)
                return status;
            if (match) {
                lxa_span_t id;
                status = lore_attribute(nodes, node, "id", &id);
                if (status != LXA_OK)
                    return status;
                if (id.length == 1 && id.data[0] == 'R') {
                    *skip = true;
                    return LXA_OK;
                }
            }
        }
        status = lxa_dom_nodes_prev_sibling(nodes, node, &node);
    }
    return status;
}

lxa_status_t bx_fetch_lore_markdown_skip_node(lxa_dom_nodes_t* nodes,
                                               lxa_dom_ref_t node, bool* skip) {
    if (!nodes || !skip)
        return LXA_ERROR_ARGUMENT;
    *skip = false;
    if (!node.handle)
        return LXA_OK;
    lxa_status_t status = lore_pre_is_junk(nodes, node, skip);
    if (status != LXA_OK || *skip)
        return status;
    status = lore_follows_reply_instructions(nodes, node, skip);
    if (status != LXA_OK || *skip)
        return status;
    lxa_dom_record_t record;
    status = lxa_dom_nodes_read(nodes, node, &record);
    if (status != LXA_OK)
        return status;
    if (record.kind == LXA_DOM_KIND_TEXT) {
        lxa_span_t text;
        status = lxa_dom_nodes_character_read(nodes, node, &text);
        if (status == LXA_OK)
            *skip = lore_text_starts_with(text, "Be sure your reply has a Subject: header");
        return status;
    }
    bool hr;
    status = lore_is_tag(nodes, node, "hr", &hr);
    if (status != LXA_OK || !hr)
        return status;
    status = lxa_dom_nodes_next_sibling(nodes, node, &node);
    while (status == LXA_OK && node.handle) {
        status = lxa_dom_nodes_read(nodes, node, &record);
        if (status != LXA_OK)
            return status;
        if (record.kind == LXA_DOM_KIND_TEXT) {
            lxa_span_t text;
            status = lxa_dom_nodes_character_read(nodes, node, &text);
            if (status != LXA_OK)
                return status;
            bool whitespace = true;
            for (size_t i = 0; i < text.length; i++) {
                if (!isspace((unsigned char)text.data[i])) {
                    whitespace = false;
                    break;
                }
            }
            if (whitespace) {
                status = lxa_dom_nodes_next_sibling(nodes, node, &node);
                continue;
            }
        }
        return lore_pre_is_junk(nodes, node, skip);
    }
    return status;
}
