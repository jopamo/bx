#include "lib/fetch/html/site_markdown.h"

#if HAVE_LEXBOR
/*
 * These rules remove stable presentation chrome from sites that do not mark
 * it up with nav, aside, footer, hidden, or ARIA roles. Keep them out of the
 * generic HTML mapping so a site's class names cannot affect other pages.
 */
#include <ctype.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <lexbor/dom/interfaces/element.h>

static const lxb_char_t* site_attribute(lxb_dom_element_t* element, const char* name, size_t* length) {
    return lxb_dom_element_get_attribute(element, (const lxb_char_t*)name, strlen(name), length);
}

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

static bool span_equals(const lxb_char_t* value, size_t length, const char* expected) {
    size_t expected_length = strlen(expected);
    return value && length == expected_length && memcmp(value, expected, length) == 0;
}

static bool element_id_equals(lxb_dom_element_t* element, const char* expected) {
    size_t length = 0;
    const lxb_char_t* id = site_attribute(element, "id", &length);
    return span_equals(id, length, expected);
}

static bool element_has_class(lxb_dom_element_t* element, const char* expected) {
    size_t length = 0;
    const lxb_char_t* classes = site_attribute(element, "class", &length);
    size_t expected_length = strlen(expected);
    size_t position = 0;
    while (classes && position < length) {
        while (position < length && isspace(classes[position]))
            position++;
        size_t end = position;
        while (end < length && !isspace(classes[end]))
            end++;
        if (end - position == expected_length && memcmp(classes + position, expected, expected_length) == 0)
            return true;
        position = end;
    }
    return false;
}

static bool node_follows_man_text(lxb_dom_node_t* node) {
    for (lxb_dom_node_t* previous = node ? node->prev : NULL; previous; previous = previous->prev) {
        if (previous->type == LXB_DOM_NODE_TYPE_ELEMENT && previous->local_name == LXB_TAG_HR &&
            element_has_class(lxb_dom_interface_element(previous), "end-man-text")) {
            return true;
        }
    }
    return false;
}

static bool skip_kernel_sphinx(lxb_dom_element_t* element) {
    return element_has_class(element, "language-selection") || element_has_class(element, "headerlink") || element_has_class(element, "footer");
}

static bool skip_man7(lxb_dom_node_t* node, lxb_dom_element_t* element) {
    return node_follows_man_text(node) || element_has_class(element, "end-man-text") || element_has_class(element, "nav-bar") ||
           element_has_class(element, "nav-end") || element_has_class(element, "sec-table") || element_has_class(element, "section-dir") ||
           element_has_class(element, "top-link") || element_has_class(element, "footer");
}

static bool skip_rfc_editor(lxb_dom_element_t* element) {
    return element_has_class(element, "pilcrow") || element_has_class(element, "toplink") || element_id_equals(element, "section-toc.1");
}

static bool skip_wikipedia(lxb_dom_element_t* element) {
    static const char* const classes[] = {
        "catlinks",
        "metadata",
        "mw-editsection",
        "mw-cite-backlink",
        "mw-jump-link",
        "mw-valign-text-top",
        "navbox",
        "noprint",
        "printfooter",
        "vector-column-end",
        "vector-column-start",
        "vector-header-container",
        "vertical-navbox",
    };
    if (element_id_equals(element, "p-lang-btn"))
        return true;
    for (size_t index = 0; index < sizeof(classes) / sizeof(classes[0]); index++) {
        if (element_has_class(element, classes[index]))
            return true;
    }
    return false;
}

bool bx_fetch_site_markdown_skip_node(BxFetchMarkdownSite site, lxb_dom_node_t* node) {
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return false;
    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    switch (site) {
        case BX_FETCH_MARKDOWN_SITE_KERNEL_SPHINX:
            return skip_kernel_sphinx(element);
        case BX_FETCH_MARKDOWN_SITE_MAN7:
            return skip_man7(node, element);
        case BX_FETCH_MARKDOWN_SITE_RFC_EDITOR:
            return skip_rfc_editor(element);
        case BX_FETCH_MARKDOWN_SITE_WIKIPEDIA:
            return skip_wikipedia(element);
        case BX_FETCH_MARKDOWN_SITE_GENERIC:
            return false;
    }
    return false;
}

bool bx_fetch_site_markdown_unwrap_node(BxFetchMarkdownSite site, lxb_dom_node_t* node) {
    return site == BX_FETCH_MARKDOWN_SITE_RFC_EDITOR && node && node->type == LXB_DOM_NODE_TYPE_ELEMENT &&
           element_has_class(lxb_dom_interface_element(node), "selfRef");
}

#else
typedef int BxFetchSiteMarkdownDisabled;
#endif
