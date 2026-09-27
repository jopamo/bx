#include "lib/fetch/html/site_markdown.h"

#include <string.h>
#include <strings.h>

static bool url_host_matches(const char* url, const char* host, bool allow_subdomains) {
    if (!url)
        return false;
    const char* authority = strstr(url, "://");
    if (!authority)
        return false;
    authority += 3;
    const char* end = authority + strcspn(authority, "/?#");
    const char* port = memchr(authority, ':', (size_t)(end - authority));
    if (port)
        end = port;
    size_t authority_length = (size_t)(end - authority);
    size_t host_length = strlen(host);
    if (authority_length == host_length)
        return strncasecmp(authority, host, host_length) == 0;
    return allow_subdomains && authority_length > host_length && authority[authority_length - host_length - 1u] == '.' &&
           strncasecmp(end - host_length, host, host_length) == 0;
}

BxFetchMarkdownSite bx_fetch_markdown_site_for_url(const char* base_url) {
    if (url_host_matches(base_url, "docs.kernel.org", false))
        return BX_FETCH_MARKDOWN_SITE_KERNEL_SPHINX;
    if (url_host_matches(base_url, "man7.org", true))
        return BX_FETCH_MARKDOWN_SITE_MAN7;
    if (url_host_matches(base_url, "rfc-editor.org", true))
        return BX_FETCH_MARKDOWN_SITE_RFC_EDITOR;
    if (url_host_matches(base_url, "wikipedia.org", true))
        return BX_FETCH_MARKDOWN_SITE_WIKIPEDIA;
    return BX_FETCH_MARKDOWN_SITE_GENERIC;
}

/*
 * Site-specific presentation chrome is bx policy. Keep this separate from
 * the parser's generic element and attribute representation.
 */
#include <ctype.h>

static bool span_equals(lxa_span_t value, const char* expected) {
    size_t length = strlen(expected);
    return value.data && value.length == length && memcmp(value.data, expected, length) == 0;
}

static lxa_status_t attribute(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                              const char* name, lxa_span_t* value) {
    lxa_dom_ref_t attr;
    lxa_span_t key;
    lxa_span_t lookup = {(const uint8_t*)name, strlen(name)};
    *value = (lxa_span_t){0};
    lxa_status_t status = lxa_dom_nodes_find_attribute(nodes, element, lookup, &attr);
    if (status != LXA_OK || !attr.handle)
        return status;
    return lxa_dom_nodes_attribute_read(nodes, attr, &key, value);
}

static lxa_status_t element_has_class(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                      const char* expected, bool* found) {
    lxa_span_t classes;
    lxa_status_t status = attribute(nodes, element, "class", &classes);
    if (status != LXA_OK)
        return status;
    size_t expected_length = strlen(expected);
    *found = false;
    for (size_t position = 0; position < classes.length;) {
        while (position < classes.length && isspace((unsigned char)classes.data[position]))
            position++;
        size_t end = position;
        while (end < classes.length && !isspace((unsigned char)classes.data[end]))
            end++;
        if (end - position == expected_length && memcmp(classes.data + position, expected, expected_length) == 0) {
            *found = true;
            break;
        }
        position = end;
    }
    return LXA_OK;
}

static lxa_status_t has_any_class(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                  const char* const* names, size_t count, bool* found) {
    *found = false;
    for (size_t i = 0; i < count && !*found; i++) {
        lxa_status_t status = element_has_class(nodes, element, names[i], found);
        if (status != LXA_OK)
            return status;
    }
    return LXA_OK;
}

static lxa_status_t node_follows_man_text(lxa_dom_nodes_t* nodes, lxa_dom_ref_t node, bool* found) {
    lxa_status_t status = lxa_dom_nodes_prev_sibling(nodes, node, &node);
    *found = false;
    while (status == LXA_OK && node.handle) {
        lxa_dom_record_t record;
        lxa_span_t name;
        status = lxa_dom_nodes_read(nodes, node, &record);
        if (status != LXA_OK)
            break;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            status = lxa_dom_nodes_element_name(nodes, node, &name);
            if (status != LXA_OK)
                break;
            if (span_equals(name, "hr")) {
                status = element_has_class(nodes, node, "end-man-text", found);
                if (status != LXA_OK || *found)
                    break;
            }
        }
        status = lxa_dom_nodes_prev_sibling(nodes, node, &node);
    }
    return status;
}

lxa_status_t bx_fetch_site_markdown_skip_node(BxFetchMarkdownSite site,
                                               lxa_dom_nodes_t* nodes, lxa_dom_ref_t node, bool* skip) {
    static const char* const kernel[] = {"language-selection", "headerlink", "footer"};
    static const char* const man7[] = {"end-man-text", "nav-bar", "nav-end", "sec-table", "section-dir",
                                      "top-link", "footer"};
    static const char* const rfc[] = {"pilcrow", "toplink"};
    static const char* const wiki[] = {"catlinks", "metadata", "mw-editsection", "mw-cite-backlink",
                                      "mw-jump-link", "mw-valign-text-top", "navbox", "noprint",
                                      "printfooter", "vector-column-end", "vector-column-start",
                                      "vector-header-container", "vertical-navbox"};
    if (!skip || !nodes)
        return LXA_ERROR_ARGUMENT;
    *skip = false;
    lxa_dom_record_t record;
    lxa_status_t status = lxa_dom_nodes_read(nodes, node, &record);
    if (status != LXA_OK || record.kind != LXA_DOM_KIND_ELEMENT)
        return status;
    lxa_span_t id;
    switch (site) {
        case BX_FETCH_MARKDOWN_SITE_KERNEL_SPHINX:
            return has_any_class(nodes, node, kernel, sizeof(kernel) / sizeof(*kernel), skip);
        case BX_FETCH_MARKDOWN_SITE_MAN7:
            status = node_follows_man_text(nodes, node, skip);
            if (status != LXA_OK || *skip)
                return status;
            return has_any_class(nodes, node, man7, sizeof(man7) / sizeof(*man7), skip);
        case BX_FETCH_MARKDOWN_SITE_RFC_EDITOR:
            status = has_any_class(nodes, node, rfc, sizeof(rfc) / sizeof(*rfc), skip);
            if (status != LXA_OK || *skip)
                return status;
            status = attribute(nodes, node, "id", &id);
            if (status == LXA_OK)
                *skip = span_equals(id, "section-toc.1");
            return status;
        case BX_FETCH_MARKDOWN_SITE_WIKIPEDIA:
            status = attribute(nodes, node, "id", &id);
            if (status != LXA_OK)
                return status;
            if (span_equals(id, "p-lang-btn")) {
                *skip = true;
                return LXA_OK;
            }
            return has_any_class(nodes, node, wiki, sizeof(wiki) / sizeof(*wiki), skip);
        case BX_FETCH_MARKDOWN_SITE_GENERIC:
            return LXA_OK;
    }
    return LXA_OK;
}

lxa_status_t bx_fetch_site_markdown_unwrap_node(BxFetchMarkdownSite site,
                                                 lxa_dom_nodes_t* nodes, lxa_dom_ref_t node, bool* unwrap) {
    if (!unwrap || !nodes)
        return LXA_ERROR_ARGUMENT;
    *unwrap = false;
    if (site != BX_FETCH_MARKDOWN_SITE_RFC_EDITOR)
        return LXA_OK;
    lxa_dom_record_t record;
    lxa_status_t status = lxa_dom_nodes_read(nodes, node, &record);
    return status != LXA_OK || record.kind != LXA_DOM_KIND_ELEMENT
        ? status : element_has_class(nodes, node, "selfRef", unwrap);
}
