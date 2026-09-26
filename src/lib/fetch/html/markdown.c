#include "lib/fetch/html.h"
#include "lib/fetch/html/lore_markdown.h"
#include "lib/fetch/html/site_markdown.h"
#include "lib/fetch/url.h"
#include "lib/markdown/writer.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if HAVE_NATIVE_HTML
#include <liblexa/html/document.h>

/* This candidate renderer fails closed on tags whose Markdown mapping has
 * not been ported. Do not select it in the shipping bx build yet. */
typedef struct {
    lxa_dom_nodes_t* nodes;
    BxMarkdownWriter* output;
    BxFetchMarkdownSite site;
    bool lore;
} NativeMarkdown;

static bool native_markdown_status(lxa_status_t status) {
    if (status == LXA_OK)
        return true;
    errno = status == LXA_ERROR_NO_MEMORY ? ENOMEM :
            status == LXA_ERROR_LIMIT || status == LXA_ERROR_OVERFLOW ? EFBIG : EINVAL;
    return false;
}

static bool native_markdown_name(lxa_span_t name, const char* expected) {
    size_t length = strlen(expected);
    return name.length == length && memcmp(name.data, expected, length) == 0;
}

static bool native_markdown_case_name(lxa_span_t value, const char* expected) {
    size_t length = strlen(expected);
    if (value.length != length)
        return false;
    for (size_t i = 0; i < length; i++) {
        if (tolower((unsigned char)value.data[i]) != tolower((unsigned char)expected[i]))
            return false;
    }
    return true;
}

static lxa_span_t native_markdown_trim(lxa_span_t value) {
    while (value.length && isspace((unsigned char)value.data[0])) {
        value.data++;
        value.length--;
    }
    while (value.length && isspace((unsigned char)value.data[value.length - 1u]))
        value.length--;
    return value;
}

static bool native_markdown_css_equals(lxa_span_t value, const char* expected) {
    value = native_markdown_trim(value);
    size_t length = strlen(expected);
    if (value.length < length
        || !native_markdown_case_name((lxa_span_t){value.data, length}, expected))
        return false;
    value.data += length;
    value.length -= length;
    value = native_markdown_trim(value);
    return !value.length || native_markdown_case_name(value, "!important");
}

static bool native_markdown_style_hidden(lxa_span_t style) {
    for (size_t pos = 0; pos < style.length;) {
        size_t end = pos;
        while (end < style.length && style.data[end] != ';') end++;
        size_t colon = pos;
        while (colon < end && style.data[colon] != ':') colon++;
        if (colon < end) {
            lxa_span_t name = native_markdown_trim(
                (lxa_span_t){style.data + pos, colon - pos});
            lxa_span_t value = native_markdown_trim(
                (lxa_span_t){style.data + colon + 1u, end - colon - 1u});
            if ((native_markdown_case_name(name, "display")
                 && native_markdown_css_equals(value, "none"))
                || (native_markdown_case_name(name, "visibility")
                    && native_markdown_css_equals(value, "hidden")))
                return true;
        }
        pos = end < style.length ? end + 1u : end;
    }
    return false;
}

static bool native_markdown_node(NativeMarkdown* context, lxa_dom_ref_t node, size_t depth);

static bool native_markdown_children(NativeMarkdown* context, lxa_dom_ref_t parent,
                                     size_t depth) {
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, parent, &child)))
        return false;
    while (child.handle) {
        if (!native_markdown_node(context, child, depth))
            return false;
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    return true;
}

static bool native_markdown_has_text(NativeMarkdown* context, lxa_dom_ref_t node,
                                     bool* present) {
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_buffer_t text;
    if (!native_markdown_status(lxa_buffer_init(&text, &allocator)))
        return false;
    lxa_status_t status = lxa_dom_nodes_text_content(
        context->nodes, node, &text, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    *present = false;
    if (status == LXA_OK) {
        for (size_t i = 0; i < text.length; i++) {
            if (!isspace((unsigned char)text.data[i])) {
                *present = true;
                break;
            }
        }
    }
    lxa_buffer_destroy(&text);
    return native_markdown_status(status);
}

static bool native_markdown_wrapped(NativeMarkdown* context, lxa_dom_ref_t node,
                                    size_t depth, const char* marker) {
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_buffer_t text;
    if (!native_markdown_status(lxa_buffer_init(&text, &allocator)))
        return false;
    lxa_status_t status = lxa_dom_nodes_text_content(
        context->nodes, node, &text, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    if (status != LXA_OK) {
        lxa_buffer_destroy(&text);
        return native_markdown_status(status);
    }
    bool content = false;
    for (size_t i = 0; i < text.length; i++) {
        if (!isspace((unsigned char)text.data[i])) {
            content = true;
            break;
        }
    }
    bool leading = text.length && isspace((unsigned char)text.data[0]);
    bool trailing = text.length && isspace((unsigned char)text.data[text.length - 1u]);
    lxa_buffer_destroy(&text);
    if (!content)
        return native_markdown_children(context, node, depth + 1);
    BxMarkdownWriter nested;
    bx_markdown_writer_init(&nested, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    NativeMarkdown nested_context = *context;
    nested_context.output = &nested;
    bool ok = native_markdown_children(&nested_context, node, depth + 1);
    size_t length = 0;
    char* fragment = ok ? bx_markdown_writer_take(&nested, &length) : NULL;
    int failure = errno;
    bx_markdown_writer_clear(&nested);
    if (!fragment) {
        errno = failure ? failure : EINVAL;
        return false;
    }
    if (length && fragment[length - 1u] == '\n')
        fragment[--length] = '\0';
    size_t marker_length = strlen(marker);
    ok = (!leading || bx_markdown_writer_text(context->output, " ", 1u))
        && bx_markdown_writer_raw(context->output, marker, marker_length)
        && bx_markdown_writer_raw(context->output, fragment, length)
        && bx_markdown_writer_raw(context->output, marker, marker_length)
        && (!trailing || bx_markdown_writer_text(context->output, " ", 1u));
    free(fragment);
    return ok;
}

static bool native_markdown_code(NativeMarkdown* context, lxa_dom_ref_t node,
                                 bool block) {
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_buffer_t text;
    if (!native_markdown_status(lxa_buffer_init(&text, &allocator)))
        return false;
    lxa_status_t status = lxa_dom_nodes_text_content(
        context->nodes, node, &text, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    if (status != LXA_OK) {
        lxa_buffer_destroy(&text);
        return native_markdown_status(status);
    }
    bool present = false;
    for (size_t i = 0; i < text.length; i++) {
        if (!isspace((unsigned char)text.data[i])) {
            present = true;
            break;
        }
    }
    if (!present) {
        lxa_buffer_destroy(&text);
        return true;
    }
    char language[65] = {0};
    status = LXA_OK;
    if (block) {
        lxa_dom_ref_t first;
        status = lxa_dom_nodes_first_child(context->nodes, node, &first);
        if (status == LXA_OK && first.handle) {
            lxa_dom_record_t record;
            status = lxa_dom_nodes_read(context->nodes, first, &record);
            if (status == LXA_OK && record.kind == LXA_DOM_KIND_ELEMENT) {
                lxa_span_t name;
                status = lxa_dom_nodes_element_name(context->nodes, first, &name);
                if (status == LXA_OK && native_markdown_name(name, "code")) {
                    lxa_span_t key = {(const uint8_t*)"class", 5};
                    lxa_dom_ref_t attr;
                    status = lxa_dom_nodes_find_attribute(context->nodes, first, key, &attr);
                    if (status == LXA_OK && attr.handle) {
                        lxa_span_t value;
                        status = lxa_dom_nodes_attribute_read(context->nodes, attr, &key, &value);
                        static const char prefix[] = "language-";
                        if (status == LXA_OK && value.length > sizeof(prefix) - 1u
                            && memcmp(value.data, prefix, sizeof(prefix) - 1u) == 0) {
                            size_t dest = 0;
                            for (size_t source = sizeof(prefix) - 1u;
                                 source < value.length && dest + 1u < sizeof(language); source++) {
                                unsigned char c = value.data[source];
                                if (!(isalnum(c) || c == '_' || c == '+' || c == '-'))
                                    break;
                                language[dest++] = (char)c;
                            }
                        }
                    }
                }
            }
        }
    }
    bool ok = native_markdown_status(status);
    if (ok)
        ok = block
            ? bx_markdown_writer_code_block(context->output, language, (const char*)text.data, text.length)
            : bx_markdown_writer_code_span(context->output, (const char*)text.data, text.length);
    lxa_buffer_destroy(&text);
    return ok;
}

static bool native_markdown_attribute(NativeMarkdown* context, lxa_dom_ref_t node,
                                       const char* name, lxa_span_t* value) {
    lxa_span_t key = {(const uint8_t*)name, strlen(name)};
    lxa_dom_ref_t attr;
    *value = (lxa_span_t){0};
    if (!native_markdown_status(lxa_dom_nodes_find_attribute(
        context->nodes, node, key, &attr)))
        return false;
    return !attr.handle || native_markdown_status(
        lxa_dom_nodes_attribute_read(context->nodes, attr, &key, value));
}

static bool native_markdown_hidden(NativeMarkdown* context, lxa_dom_ref_t node,
                                   bool* hidden) {
    lxa_span_t key = {(const uint8_t*)"hidden", 6};
    lxa_dom_ref_t attr;
    *hidden = false;
    if (!native_markdown_status(lxa_dom_nodes_find_attribute(
        context->nodes, node, key, &attr)))
        return false;
    if (attr.handle) {
        *hidden = true;
        return true;
    }
    lxa_span_t value;
    if (!native_markdown_attribute(context, node, "aria-hidden", &value))
        return false;
    if (value.data && native_markdown_case_name(native_markdown_trim(value), "true")) {
        *hidden = true;
        return true;
    }
    if (!native_markdown_attribute(context, node, "style", &value))
        return false;
    if (value.data && native_markdown_style_hidden(value)) {
        *hidden = true;
        return true;
    }
    if (!native_markdown_attribute(context, node, "role", &value))
        return false;
    if (value.data) {
        value = native_markdown_trim(value);
        static const char* const chrome[] = {
            "navigation", "banner", "contentinfo", "complementary", "search"
        };
        for (size_t i = 0; i < sizeof(chrome) / sizeof(*chrome); i++) {
            if (native_markdown_case_name(value, chrome[i])) {
                *hidden = true;
                return true;
            }
        }
    }
    return true;
}

static bool native_markdown_destination(BxMarkdownWriter* output, lxa_span_t value) {
    bool angle = false;
    for (size_t i = 0; i < value.length; i++) {
        if (isspace((unsigned char)value.data[i]) || value.data[i] == '(' || value.data[i] == ')') {
            angle = true;
            break;
        }
    }
    if (angle && !bx_markdown_writer_raw(output, "<", 1u))
        return false;
    for (size_t i = 0; i < value.length; i++) {
        char c = (char)value.data[i];
        if ((c == '\\' || (!angle && c == ')') || (angle && c == '>'))
            && !bx_markdown_writer_raw(output, "\\", 1u))
            return false;
        if (!bx_markdown_writer_raw(output, &c, 1u))
            return false;
    }
    return !angle || bx_markdown_writer_raw(output, ">", 1u);
}

static char* native_markdown_fragment(NativeMarkdown* context, lxa_dom_ref_t node,
                                      size_t depth, size_t* length) {
    BxMarkdownWriter nested;
    bx_markdown_writer_init(&nested, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    NativeMarkdown nested_context = *context;
    nested_context.output = &nested;
    bool rendered = native_markdown_children(&nested_context, node, depth + 1);
    char* fragment = rendered ? bx_markdown_writer_take(&nested, length) : NULL;
    int failure = errno;
    bx_markdown_writer_clear(&nested);
    if (!fragment) {
        errno = failure ? failure : EINVAL;
        return NULL;
    }
    if (*length && fragment[*length - 1u] == '\n')
        fragment[--*length] = '\0';
    return fragment;
}

static bool native_markdown_link(NativeMarkdown* context, lxa_dom_ref_t node,
                                 size_t depth) {
    lxa_span_t href;
    if (!native_markdown_attribute(context, node, "href", &href))
        return false;
    if (!href.data || !href.length)
        return native_markdown_children(context, node, depth + 1);
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_buffer_t text;
    if (!native_markdown_status(lxa_buffer_init(&text, &allocator)))
        return false;
    lxa_status_t status = lxa_dom_nodes_text_content(
        context->nodes, node, &text, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    bool leading = text.length && isspace((unsigned char)text.data[0]);
    bool trailing = text.length && isspace((unsigned char)text.data[text.length - 1u]);
    lxa_buffer_destroy(&text);
    if (!native_markdown_status(status))
        return false;
    size_t length = 0;
    char* label = native_markdown_fragment(context, node, depth, &length);
    if (!label)
        return false;
    bool ok = true;
    if (length)
        ok = (!leading || bx_markdown_writer_text(context->output, " ", 1u))
            && bx_markdown_writer_raw(context->output, "[", 1u)
            && bx_markdown_writer_raw(context->output, label, length)
            && bx_markdown_writer_raw(context->output, "](", 2u)
            && native_markdown_destination(context->output, href)
            && bx_markdown_writer_raw(context->output, ")", 1u)
            && (!trailing || bx_markdown_writer_text(context->output, " ", 1u));
    free(label);
    return ok;
}

static bool native_markdown_image(NativeMarkdown* context, lxa_dom_ref_t node) {
    lxa_span_t src, alt;
    if (!native_markdown_attribute(context, node, "src", &src))
        return false;
    if (!src.data || !src.length)
        return true;
    if (!native_markdown_attribute(context, node, "alt", &alt))
        return false;
    bool content = false;
    for (size_t i = 0; i < alt.length; i++) {
        if (!isspace((unsigned char)alt.data[i])) {
            content = true;
            break;
        }
    }
    if (!content)
        return true;
    return bx_markdown_writer_raw(context->output, "![", 2u)
        && bx_markdown_writer_text(context->output, (const char*)alt.data, alt.length)
        && bx_markdown_writer_raw(context->output, "](", 2u)
        && native_markdown_destination(context->output, src)
        && bx_markdown_writer_raw(context->output, ")", 1u);
}

static bool native_markdown_node(NativeMarkdown* context, lxa_dom_ref_t node, size_t depth) {
    if (depth > 256) {
        errno = EFBIG;
        return false;
    }
    bool skip = false, unwrap = false;
    if (context->lore && !native_markdown_status(
        bx_fetch_lore_markdown_skip_node(context->nodes, node, &skip)))
        return false;
    if (skip)
        return true;
    if (!native_markdown_status(bx_fetch_site_markdown_skip_node(
        context->site, context->nodes, node, &skip)))
        return false;
    if (skip)
        return true;
    if (!native_markdown_status(bx_fetch_site_markdown_unwrap_node(
        context->site, context->nodes, node, &unwrap)))
        return false;
    lxa_dom_record_t record;
    if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, node, &record)))
        return false;
    if (record.kind == LXA_DOM_KIND_TEXT) {
        lxa_span_t text;
        return native_markdown_status(lxa_dom_nodes_character_read(context->nodes, node, &text))
            && bx_markdown_writer_text(context->output, (const char*)text.data, text.length);
    }
    if (record.kind != LXA_DOM_KIND_ELEMENT)
        return true;
    lxa_span_t name;
    if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, node, &name)))
        return false;
    if (native_markdown_name(name, "head") || native_markdown_name(name, "script")
        || native_markdown_name(name, "style") || native_markdown_name(name, "template")
        || native_markdown_name(name, "noscript") || native_markdown_name(name, "nav")
        || native_markdown_name(name, "aside") || native_markdown_name(name, "footer")
        || native_markdown_name(name, "svg") || native_markdown_name(name, "canvas")
        || native_markdown_name(name, "iframe") || native_markdown_name(name, "form")
        || native_markdown_name(name, "button") || native_markdown_name(name, "input")
        || native_markdown_name(name, "select") || native_markdown_name(name, "textarea"))
        return true;
    bool hidden;
    if (!native_markdown_hidden(context, node, &hidden))
        return false;
    if (hidden)
        return true;
    if (unwrap || native_markdown_name(name, "span") || native_markdown_name(name, "body"))
        return native_markdown_children(context, node, depth + 1);
    if (name.length == 2 && name.data[0] == 'h' && name.data[1] >= '1' && name.data[1] <= '6') {
        bool present;
        if (!native_markdown_has_text(context, node, &present))
            return false;
        if (!present)
            return true;
        static const char hashes[] = "######";
        size_t level = (size_t)(name.data[1] - '0');
        return bx_markdown_writer_newlines(context->output, 2u)
            && bx_markdown_writer_raw(context->output, hashes, level)
            && bx_markdown_writer_raw(context->output, " ", 1u)
            && native_markdown_children(context, node, depth + 1)
            && bx_markdown_writer_newlines(context->output, 2u);
    }
    if (native_markdown_name(name, "p") || native_markdown_name(name, "div")
        || native_markdown_name(name, "section") || native_markdown_name(name, "article")
        || native_markdown_name(name, "main") || native_markdown_name(name, "header")) {
        return bx_markdown_writer_newlines(context->output, 2u)
            && native_markdown_children(context, node, depth + 1)
            && bx_markdown_writer_newlines(context->output, 2u);
    }
    if (native_markdown_name(name, "br"))
        return bx_markdown_writer_raw(context->output, "\\\n", 2u);
    if (native_markdown_name(name, "strong") || native_markdown_name(name, "b"))
        return native_markdown_wrapped(context, node, depth, "**");
    if (native_markdown_name(name, "em") || native_markdown_name(name, "i"))
        return native_markdown_wrapped(context, node, depth, "*");
    if (native_markdown_name(name, "del") || native_markdown_name(name, "s")
        || native_markdown_name(name, "strike"))
        return native_markdown_wrapped(context, node, depth, "~~");
    if (native_markdown_name(name, "code") || native_markdown_name(name, "samp")
        || native_markdown_name(name, "kbd"))
        return native_markdown_code(context, node, false);
    if (native_markdown_name(name, "pre") || native_markdown_name(name, "listing"))
        return native_markdown_code(context, node, true);
    if (native_markdown_name(name, "a"))
        return native_markdown_link(context, node, depth);
    if (native_markdown_name(name, "img") || native_markdown_name(name, "image"))
        return native_markdown_image(context, node);
    /* Lists and tables need their
     * own native mappings before this path can be enabled for bx. */
    errno = ENOTSUP;
    return false;
}

int bx_fetch_html_markdown_supported(void) {
    return 0; /* A partial candidate is not a supported bx Markdown path. */
}

char* bx_fetch_html_to_markdown(const char* base_url, const char* html_data,
                               size_t len, bool absolute_links, size_t* output_len) {
    if (output_len) *output_len = 0;
    if (!html_data) {
        errno = EINVAL;
        return NULL;
    }
    if (len > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES) {
        errno = EFBIG;
        return NULL;
    }
    if (absolute_links) {
        errno = ENOTSUP;
        return NULL;
    }
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_limits_t limits = lxa_limits_default();
    limits.max_input_bytes = BX_FETCH_DOCUMENT_PARSE_MAX_BYTES;
    lxa_html_document_t* document = NULL;
    lxa_span_t input = {(const uint8_t*)html_data, len};
    if (!native_markdown_status(lxa_html_document_parse(
        &allocator, &limits, input, NULL, NULL, &document)))
        return NULL;
    errno = 0;
    BxMarkdownWriter output;
    bx_markdown_writer_init(&output, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    NativeMarkdown context = {
        .nodes = lxa_html_document_nodes(document),
        .output = &output,
        .site = bx_fetch_markdown_site_for_url(base_url),
        .lore = bx_fetch_lore_markdown_url_matches(base_url),
    };
    /* Title fallback has not been ported. Reject a titled document instead
     * of quietly omitting its heading. */
    lxa_dom_ref_t head = lxa_html_document_head(document), item = {0};
    bool ready = !head.handle || native_markdown_status(
        lxa_dom_nodes_first_child(context.nodes, head, &item));
    while (ready && item.handle) {
        lxa_dom_record_t record;
        ready = native_markdown_status(lxa_dom_nodes_read(context.nodes, item, &record));
        if (!ready) break;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            lxa_span_t name;
            ready = native_markdown_status(lxa_dom_nodes_element_name(context.nodes, item, &name));
            if (!ready) break;
            if (native_markdown_name(name, "title")) {
                errno = ENOTSUP;
                ready = false;
                break;
            }
        }
        lxa_dom_ref_t next;
        ready = native_markdown_status(lxa_dom_nodes_next_sibling(context.nodes, item, &next));
        item = next;
    }
    lxa_dom_ref_t body = lxa_html_document_body(document);
    bool rendered = ready && body.handle
        && native_markdown_children(&context, body, 0);
    char* result = rendered ? bx_markdown_writer_take(&output, output_len) : NULL;
    int failure = errno;
    bx_markdown_writer_clear(&output);
    lxa_html_document_destroy(document);
    if (!result) errno = failure ? failure : EINVAL;
    return result;
}

#elif HAVE_LEXBOR
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/html.h>

typedef struct {
    BxMarkdownWriter* output;
    const char* link_base;
    size_t list_depth;
    size_t ordered_index[32];
    bool ordered[32];
    bool in_list_item;
    bool lore_kernel_org;
    BxFetchMarkdownSite site;
} HtmlMarkdownContext;

static bool render_node(HtmlMarkdownContext* context, lxb_dom_node_t* node);

static bool render_children(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    for (lxb_dom_node_t* child = node ? node->first_child : NULL; child; child = child->next) {
        if (!render_node(context, child))
            return false;
    }
    return true;
}

static const lxb_char_t* element_attribute(lxb_dom_element_t* element, const char* name, size_t* length) {
    return lxb_dom_element_get_attribute(element, (const lxb_char_t*)name, strlen(name), length);
}

static bool span_ascii_case_equals(const lxb_char_t* value, size_t length, const char* expected) {
    size_t expected_length = strlen(expected);
    if (!value || length != expected_length)
        return false;
    for (size_t index = 0; index < length; index++) {
        if (tolower(value[index]) != tolower((unsigned char)expected[index]))
            return false;
    }
    return true;
}

static void trim_ascii_whitespace(const lxb_char_t** value, size_t* length) {
    while (*length > 0 && isspace((*value)[0])) {
        (*value)++;
        (*length)--;
    }
    while (*length > 0 && isspace((*value)[*length - 1u]))
        (*length)--;
}

static bool css_value_equals(const lxb_char_t* value, size_t length, const char* expected) {
    trim_ascii_whitespace(&value, &length);
    size_t expected_length = strlen(expected);
    if (length < expected_length || !span_ascii_case_equals(value, expected_length, expected))
        return false;
    value += expected_length;
    length -= expected_length;
    trim_ascii_whitespace(&value, &length);
    static const char important[] = "!important";
    return length == 0 || span_ascii_case_equals(value, length, important);
}

static bool style_hides_element(const lxb_char_t* style, size_t length) {
    size_t position = 0;
    while (position < length) {
        size_t end = position;
        while (end < length && style[end] != ';')
            end++;
        size_t colon = position;
        while (colon < end && style[colon] != ':')
            colon++;
        if (colon < end) {
            const lxb_char_t* name = style + position;
            size_t name_length = colon - position;
            const lxb_char_t* value = style + colon + 1u;
            size_t value_length = end - colon - 1u;
            trim_ascii_whitespace(&name, &name_length);
            trim_ascii_whitespace(&value, &value_length);
            if ((span_ascii_case_equals(name, name_length, "display") && css_value_equals(value, value_length, "none")) ||
                (span_ascii_case_equals(name, name_length, "visibility") && css_value_equals(value, value_length, "hidden"))) {
                return true;
            }
        }
        position = end < length ? end + 1u : end;
    }
    return false;
}

static bool element_is_hidden(lxb_dom_element_t* element) {
    size_t length = 0;
    if (lxb_dom_element_has_attribute(element, (const lxb_char_t*)"hidden", 6u))
        return true;

    const lxb_char_t* value = element_attribute(element, "aria-hidden", &length);
    if (value) {
        trim_ascii_whitespace(&value, &length);
        if (span_ascii_case_equals(value, length, "true"))
            return true;
    }

    value = element_attribute(element, "style", &length);
    return value && style_hides_element(value, length);
}

static bool element_has_chrome_role(lxb_dom_element_t* element) {
    size_t length = 0;
    const lxb_char_t* role = element_attribute(element, "role", &length);
    if (!role)
        return false;
    trim_ascii_whitespace(&role, &length);
    return span_ascii_case_equals(role, length, "navigation") || span_ascii_case_equals(role, length, "banner") || span_ascii_case_equals(role, length, "contentinfo") ||
           span_ascii_case_equals(role, length, "complementary") || span_ascii_case_equals(role, length, "search");
}

static bool element_has_ignored_tag(lxb_dom_node_t* node) {
    switch ((lxb_tag_id_t)node->local_name) {
        case LXB_TAG_HEAD:
        case LXB_TAG_SCRIPT:
        case LXB_TAG_STYLE:
        case LXB_TAG_TEMPLATE:
        case LXB_TAG_NOSCRIPT:
        case LXB_TAG_SVG:
        case LXB_TAG_CANVAS:
        case LXB_TAG_IFRAME:
        case LXB_TAG_NAV:
        case LXB_TAG_ASIDE:
        case LXB_TAG_FOOTER:
        case LXB_TAG_FORM:
        case LXB_TAG_BUTTON:
        case LXB_TAG_INPUT:
        case LXB_TAG_SELECT:
        case LXB_TAG_TEXTAREA:
            return true;
        default:
            return false;
    }
}

static bool text_has_content(const lxb_char_t* text, size_t length) {
    for (size_t index = 0; index < length; index++) {
        if (!isspace(text[index]))
            return true;
    }
    return false;
}

static bool node_has_text(lxb_dom_node_t* node) {
    size_t length = 0;
    lxb_char_t* text = lxb_dom_node_text_content(node, &length);
    bool present = text && text_has_content(text, length);
    if (text)
        lxb_dom_document_destroy_text(node->owner_document, text);
    return present;
}

static char* render_children_fragment(const HtmlMarkdownContext* context, lxb_dom_node_t* node, size_t* length_out) {
    BxMarkdownWriter nested;
    bx_markdown_writer_init(&nested, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    HtmlMarkdownContext nested_context = *context;
    nested_context.output = &nested;
    bool rendered = render_children(&nested_context, node);
    size_t length = 0;
    char* fragment = rendered ? bx_markdown_writer_take(&nested, &length) : NULL;
    int error_number = errno;
    bx_markdown_writer_clear(&nested);
    if (!fragment) {
        errno = error_number ? error_number : EINVAL;
        return NULL;
    }
    if (length > 0 && fragment[length - 1u] == '\n')
        fragment[--length] = '\0';
    if (length_out)
        *length_out = length;
    return fragment;
}

static bool node_text_edge_whitespace(lxb_dom_node_t* node, bool* leading, bool* trailing) {
    size_t length = 0;
    lxb_char_t* text = lxb_dom_node_text_content(node, &length);
    if (!text)
        return length == 0;
    *leading = length > 0 && isspace(text[0]);
    *trailing = length > 0 && isspace(text[length - 1u]);
    lxb_dom_document_destroy_text(node->owner_document, text);
    return true;
}

static bool render_wrapped(HtmlMarkdownContext* context, lxb_dom_node_t* node, const char* marker) {
    if (!node_has_text(node))
        return render_children(context, node);
    bool leading = false;
    bool trailing = false;
    if (!node_text_edge_whitespace(node, &leading, &trailing))
        return false;
    size_t length = 0;
    char* fragment = render_children_fragment(context, node, &length);
    if (!fragment)
        return false;
    bool rendered = (!leading || bx_markdown_writer_text(context->output, " ", 1u)) && bx_markdown_writer_raw(context->output, marker, strlen(marker)) &&
                    bx_markdown_writer_raw(context->output, fragment, length) && bx_markdown_writer_raw(context->output, marker, strlen(marker)) &&
                    (!trailing || bx_markdown_writer_text(context->output, " ", 1u));
    free(fragment);
    return rendered;
}

static bool render_block(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    if (context->in_list_item)
        return render_children(context, node) && bx_markdown_writer_newlines(context->output, 1u);
    return bx_markdown_writer_newlines(context->output, 2u) && render_children(context, node) && bx_markdown_writer_newlines(context->output, 2u);
}

static bool render_heading(HtmlMarkdownContext* context, lxb_dom_node_t* node, size_t level) {
    if (!node_has_text(node))
        return true;
    static const char hashes[] = "######";
    return bx_markdown_writer_newlines(context->output, 2u) && bx_markdown_writer_raw(context->output, hashes, level) && bx_markdown_writer_raw(context->output, " ", 1u) &&
           render_children(context, node) && bx_markdown_writer_newlines(context->output, 2u);
}

static bool render_code(HtmlMarkdownContext* context, lxb_dom_node_t* node, bool block) {
    size_t length = 0;
    lxb_char_t* text = lxb_dom_node_text_content(node, &length);
    if (!text)
        return length == 0;
    if (!text_has_content(text, length)) {
        lxb_dom_document_destroy_text(node->owner_document, text);
        return true;
    }

    bool result;
    if (block) {
        char language[65] = {0};
        lxb_dom_node_t* code = node->first_child;
        if (code && code->type == LXB_DOM_NODE_TYPE_ELEMENT && code->local_name == LXB_TAG_CODE) {
            size_t class_length = 0;
            const lxb_char_t* class_name = element_attribute(lxb_dom_interface_element(code), "class", &class_length);
            static const char prefix[] = "language-";
            if (class_name && class_length > sizeof(prefix) - 1u && memcmp(class_name, prefix, sizeof(prefix) - 1u) == 0) {
                size_t source = sizeof(prefix) - 1u;
                size_t destination = 0;
                while (source < class_length && destination + 1u < sizeof(language)) {
                    unsigned char c = class_name[source++];
                    if (!(isalnum(c) || c == '_' || c == '+' || c == '-'))
                        break;
                    language[destination++] = (char)c;
                }
            }
        }
        result = bx_markdown_writer_code_block(context->output, language, (const char*)text, length);
    }
    else {
        result = bx_markdown_writer_code_span(context->output, (const char*)text, length);
    }
    lxb_dom_document_destroy_text(node->owner_document, text);
    return result;
}

static bool render_link_destination(BxMarkdownWriter* output, const lxb_char_t* value, size_t length) {
    bool angle = false;
    for (size_t index = 0; index < length; index++) {
        if (isspace(value[index]) || value[index] == '(' || value[index] == ')') {
            angle = true;
            break;
        }
    }
    if (angle && !bx_markdown_writer_raw(output, "<", 1u))
        return false;
    for (size_t index = 0; index < length; index++) {
        if ((value[index] == '\\' || (!angle && value[index] == ')') || (angle && value[index] == '>')) && !bx_markdown_writer_raw(output, "\\", 1u)) {
            return false;
        }
        if (!bx_markdown_writer_raw(output, (const char*)&value[index], 1u))
            return false;
    }
    return !angle || bx_markdown_writer_raw(output, ">", 1u);
}

static bool render_destination(HtmlMarkdownContext* context, const lxb_char_t* value, size_t length) {
    char* resolved = NULL;
    if (context->link_base) {
        char* reference = strndup((const char*)value, length);
        if (!reference)
            return false;
        /* Keep explicit schemes (including mailto:) and unresolvable
         * references intact; resolution must not initiate a request. */
        errno = 0;
        if (!bx_fetch_url_has_explicit_scheme(reference))
            resolved = bx_fetch_url_resolve(context->link_base, reference);
        int error_number = errno;
        free(reference);
        if (!resolved && error_number == ENOMEM)
            return false;
    }
    bool rendered = render_link_destination(context->output,
                                            resolved ? (const lxb_char_t*)resolved : value,
                                            resolved ? strlen(resolved) : length);
    free(resolved);
    return rendered;
}

static bool render_link(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    size_t href_length = 0;
    const lxb_char_t* href = element_attribute(element, "href", &href_length);
    if (!href || href_length == 0)
        return render_children(context, node);
    bool leading = false;
    bool trailing = false;
    if (!node_text_edge_whitespace(node, &leading, &trailing))
        return false;
    size_t label_length = 0;
    char* label = render_children_fragment(context, node, &label_length);
    if (!label)
        return false;
    if (label_length == 0) {
        free(label);
        return true;
    }
    bool rendered = (!leading || bx_markdown_writer_text(context->output, " ", 1u)) && bx_markdown_writer_raw(context->output, "[", 1u) &&
                    bx_markdown_writer_raw(context->output, label, label_length) && bx_markdown_writer_raw(context->output, "](", 2u) && render_destination(context, href, href_length) &&
                    bx_markdown_writer_raw(context->output, ")", 1u) && (!trailing || bx_markdown_writer_text(context->output, " ", 1u));
    free(label);
    return rendered;
}

static bool render_image(HtmlMarkdownContext* context, lxb_dom_element_t* element) {
    size_t source_length = 0;
    const lxb_char_t* source = element_attribute(element, "src", &source_length);
    if (!source || source_length == 0)
        return true;
    size_t alt_length = 0;
    const lxb_char_t* alt = element_attribute(element, "alt", &alt_length);
    if (!alt || !text_has_content(alt, alt_length))
        return true;
    return bx_markdown_writer_raw(context->output, "![", 2u) && bx_markdown_writer_text(context->output, (const char*)alt, alt_length) && bx_markdown_writer_raw(context->output, "](", 2u) &&
           render_destination(context, source, source_length) && bx_markdown_writer_raw(context->output, ")", 1u);
}

static bool render_list(HtmlMarkdownContext* context, lxb_dom_node_t* node, bool ordered) {
    if (context->list_depth >= sizeof(context->ordered) / sizeof(context->ordered[0])) {
        errno = EFBIG;
        return false;
    }
    if (!bx_markdown_writer_newlines(context->output, context->list_depth == 0 ? 2u : 1u))
        return false;
    size_t depth = context->list_depth++;
    context->ordered[depth] = ordered;
    context->ordered_index[depth] = 1u;
    if (ordered) {
        size_t start_length = 0;
        const lxb_char_t* start = element_attribute(lxb_dom_interface_element(node), "start", &start_length);
        size_t parsed = 0;
        bool valid = start && start_length > 0;
        for (size_t index = 0; valid && index < start_length; index++) {
            if (!isdigit(start[index]) || parsed > (SIZE_MAX - (size_t)(start[index] - '0')) / 10u)
                valid = false;
            else
                parsed = parsed * 10u + (size_t)(start[index] - '0');
        }
        if (valid)
            context->ordered_index[depth] = parsed;
    }
    bool result = render_children(context, node);
    context->list_depth--;
    return result && bx_markdown_writer_newlines(context->output, context->list_depth == 0 ? 2u : 1u);
}

static bool render_list_item(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    if (context->list_depth == 0)
        return render_block(context, node);
    size_t depth = context->list_depth - 1u;
    if (!bx_markdown_writer_newlines(context->output, 1u))
        return false;
    for (size_t index = 0; index < depth; index++) {
        if (!bx_markdown_writer_raw(context->output, "  ", 2u))
            return false;
    }
    if (context->ordered[depth]) {
        char prefix[32];
        int length = snprintf(prefix, sizeof(prefix), "%zu. ", context->ordered_index[depth]++);
        if (length < 0 || (size_t)length >= sizeof(prefix) || !bx_markdown_writer_raw(context->output, prefix, (size_t)length))
            return false;
    }
    else if (!bx_markdown_writer_raw(context->output, "- ", 2u)) {
        return false;
    }
    bool was_in_list_item = context->in_list_item;
    context->in_list_item = true;
    bool result = render_children(context, node);
    context->in_list_item = was_in_list_item;
    return result;
}

static bool render_blockquote(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    BxMarkdownWriter nested;
    bx_markdown_writer_init(&nested, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    HtmlMarkdownContext nested_context = *context;
    nested_context.output = &nested;
    bool rendered = render_children(&nested_context, node);
    size_t length = 0;
    char* text = rendered ? bx_markdown_writer_take(&nested, &length) : NULL;
    int error_number = errno;
    bx_markdown_writer_clear(&nested);
    if (!text) {
        errno = error_number ? error_number : EINVAL;
        return false;
    }

    bool result = bx_markdown_writer_newlines(context->output, 2u);
    size_t start = 0;
    while (result && start < length) {
        size_t end = start;
        while (end < length && text[end] != '\n')
            end++;
        result = bx_markdown_writer_raw(context->output, "> ", 2u) && bx_markdown_writer_raw(context->output, text + start, end - start) && bx_markdown_writer_newlines(context->output, 1u);
        start = end < length ? end + 1u : end;
    }
    free(text);
    return result && bx_markdown_writer_newlines(context->output, 2u);
}

static bool render_table_cell_text(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t* text = lxb_dom_interface_character_data(node);
        return bx_markdown_writer_text(context->output, (const char*)text->data.data, text->data.length);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return true;
    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    if (element_is_hidden(element) || element_has_chrome_role(element) || element_has_ignored_tag(node) ||
        bx_fetch_site_markdown_skip_node(context->site, node)) {
        return true;
    }
    if (node->local_name == LXB_TAG_IMG || node->local_name == LXB_TAG_IMAGE) {
        return true;
    }
    if (node->local_name == LXB_TAG_BR && !bx_markdown_writer_text(context->output, " ", 1u))
        return false;
    for (lxb_dom_node_t* child = node->first_child; child; child = child->next) {
        if (!render_table_cell_text(context, child))
            return false;
    }
    return true;
}

static size_t table_cell_span(lxb_dom_element_t* cell) {
    size_t length = 0;
    const lxb_char_t* value = element_attribute(cell, "colspan", &length);
    size_t span = 0;
    if (!value || length == 0)
        return 1u;
    for (size_t index = 0; index < length; index++) {
        if (!isdigit(value[index]) || span > (SIZE_MAX - (size_t)(value[index] - '0')) / 10u)
            return 1u;
        span = span * 10u + (size_t)(value[index] - '0');
    }
    return span > 0 && span <= 1000u ? span : 1u;
}

static bool table_cell_is_visible(HtmlMarkdownContext* context, lxb_dom_node_t* cell) {
    return cell->type == LXB_DOM_NODE_TYPE_ELEMENT && (cell->local_name == LXB_TAG_TH || cell->local_name == LXB_TAG_TD) &&
           !element_is_hidden(lxb_dom_interface_element(cell)) && !bx_fetch_site_markdown_skip_node(context->site, cell);
}

static size_t table_row_columns(HtmlMarkdownContext* context, lxb_dom_node_t* row) {
    size_t columns = 0;
    for (lxb_dom_node_t* cell = row->first_child; cell; cell = cell->next) {
        if (table_cell_is_visible(context, cell))
            columns += table_cell_span(lxb_dom_interface_element(cell));
    }
    return columns;
}

static size_t table_column_count(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    size_t columns = 0;
    for (lxb_dom_node_t* child = node->first_child; child; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || element_is_hidden(lxb_dom_interface_element(child)) ||
            bx_fetch_site_markdown_skip_node(context->site, child)) {
            continue;
        }
        size_t child_columns = 0;
        if (child->local_name == LXB_TAG_TR)
            child_columns = table_row_columns(context, child);
        else if (child->local_name == LXB_TAG_THEAD || child->local_name == LXB_TAG_TBODY || child->local_name == LXB_TAG_TFOOT)
            child_columns = table_column_count(context, child);
        if (child_columns > columns)
            columns = child_columns;
    }
    return columns;
}

static bool render_table_row(HtmlMarkdownContext* context, lxb_dom_node_t* row, size_t columns, bool first_row) {
    size_t cell_count = table_row_columns(context, row);
    if (cell_count == 0)
        return true;
    if (!bx_markdown_writer_raw(context->output, "|", 1u))
        return false;
    for (lxb_dom_node_t* cell = row->first_child; cell; cell = cell->next) {
        if (!table_cell_is_visible(context, cell)) {
            continue;
        }
        if (!bx_markdown_writer_raw(context->output, " ", 1u) || !render_table_cell_text(context, cell) || !bx_markdown_writer_raw(context->output, " |", 2u))
            return false;
        size_t span = table_cell_span(lxb_dom_interface_element(cell));
        for (size_t index = 1u; index < span; index++) {
            if (!bx_markdown_writer_raw(context->output, "  |", 3u))
                return false;
        }
    }
    for (size_t index = cell_count; index < columns; index++) {
        if (!bx_markdown_writer_raw(context->output, "  |", 3u))
            return false;
    }
    if (!bx_markdown_writer_newlines(context->output, 1u))
        return false;
    if (!first_row)
        return true;
    for (size_t index = 0; index < columns; index++) {
        if (!bx_markdown_writer_raw(context->output, "| --- ", 6u))
            return false;
    }
    return bx_markdown_writer_raw(context->output, "|", 1u) && bx_markdown_writer_newlines(context->output, 1u);
}

static bool render_table_rows(HtmlMarkdownContext* context, lxb_dom_node_t* node, size_t columns, bool* first_row) {
    for (lxb_dom_node_t* child = node->first_child; child; child = child->next) {
        if (child->type != LXB_DOM_NODE_TYPE_ELEMENT || element_is_hidden(lxb_dom_interface_element(child)))
            continue;
        if (child->local_name == LXB_TAG_TR) {
            size_t row_columns = table_row_columns(context, child);
            if (!render_table_row(context, child, columns, *first_row))
                return false;
            if (row_columns > 0)
                *first_row = false;
        }
        else if (child->local_name == LXB_TAG_THEAD || child->local_name == LXB_TAG_TBODY || child->local_name == LXB_TAG_TFOOT) {
            if (!render_table_rows(context, child, columns, first_row))
                return false;
        }
    }
    return true;
}

static bool render_table(HtmlMarkdownContext* context, lxb_dom_node_t* table) {
    size_t columns = table_column_count(context, table);
    if (columns == 0)
        return true;
    if (!bx_markdown_writer_newlines(context->output, 2u))
        return false;
    for (lxb_dom_node_t* child = table->first_child; child; child = child->next) {
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT && child->local_name == LXB_TAG_CAPTION && !element_is_hidden(lxb_dom_interface_element(child))) {
            if (!render_wrapped(context, child, "**") || !bx_markdown_writer_newlines(context->output, 1u))
                return false;
        }
    }
    bool first_row = true;
    return render_table_rows(context, table, columns, &first_row) && bx_markdown_writer_newlines(context->output, 2u);
}

static bool render_node(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    if (!context || !node)
        return false;
    if (context->lore_kernel_org && bx_fetch_lore_markdown_skip_node(node))
        return true;
    if (bx_fetch_site_markdown_skip_node(context->site, node))
        return true;
    if (bx_fetch_site_markdown_unwrap_node(context->site, node))
        return render_children(context, node);
    if (node->type == LXB_DOM_NODE_TYPE_TEXT) {
        lxb_dom_character_data_t* text = lxb_dom_interface_character_data(node);
        return bx_markdown_writer_text(context->output, (const char*)text->data.data, text->data.length);
    }
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return true;
    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    if (element_is_hidden(element) || element_has_chrome_role(element) || element_has_ignored_tag(node)) {
        return true;
    }

    switch ((lxb_tag_id_t)node->local_name) {
        case LXB_TAG_H1:
        case LXB_TAG_H2:
        case LXB_TAG_H3:
        case LXB_TAG_H4:
        case LXB_TAG_H5:
        case LXB_TAG_H6:
            return render_heading(context, node, (size_t)(node->local_name - LXB_TAG_H1 + 1u));
        case LXB_TAG_P:
        case LXB_TAG_DIV:
        case LXB_TAG_SECTION:
        case LXB_TAG_ARTICLE:
        case LXB_TAG_MAIN:
        case LXB_TAG_HEADER:
        case LXB_TAG_FIGURE:
        case LXB_TAG_FIGCAPTION:
        case LXB_TAG_DETAILS:
        case LXB_TAG_SUMMARY:
            return render_block(context, node);
        case LXB_TAG_BR:
            return bx_markdown_writer_raw(context->output, "\\\n", 2u);
        case LXB_TAG_HR:
            return bx_markdown_writer_newlines(context->output, 2u) && bx_markdown_writer_raw(context->output, "---", 3u) && bx_markdown_writer_newlines(context->output, 2u);
        case LXB_TAG_STRONG:
        case LXB_TAG_B:
            return render_wrapped(context, node, "**");
        case LXB_TAG_EM:
        case LXB_TAG_I:
            return render_wrapped(context, node, "*");
        case LXB_TAG_DEL:
        case LXB_TAG_S:
        case LXB_TAG_STRIKE:
            return render_wrapped(context, node, "~~");
        case LXB_TAG_CODE:
        case LXB_TAG_SAMP:
        case LXB_TAG_KBD:
            return render_code(context, node, false);
        case LXB_TAG_PRE:
        case LXB_TAG_LISTING:
            return render_code(context, node, true);
        case LXB_TAG_A:
            return render_link(context, node);
        case LXB_TAG_IMG:
        case LXB_TAG_IMAGE:
            return render_image(context, lxb_dom_interface_element(node));
        case LXB_TAG_UL:
            return render_list(context, node, false);
        case LXB_TAG_OL:
            return render_list(context, node, true);
        case LXB_TAG_LI:
            return render_list_item(context, node);
        case LXB_TAG_BLOCKQUOTE:
            return render_blockquote(context, node);
        case LXB_TAG_DL:
            return render_block(context, node);
        case LXB_TAG_DT:
            return bx_markdown_writer_newlines(context->output, 1u) && render_wrapped(context, node, "**");
        case LXB_TAG_DD:
            return bx_markdown_writer_newlines(context->output, 1u) && bx_markdown_writer_raw(context->output, ": ", 2u) && render_children(context, node);
        case LXB_TAG_TABLE:
            return render_table(context, node);
        default:
            return render_children(context, node);
    }
}

static lxb_dom_node_t* find_element(lxb_dom_node_t* node, lxb_tag_id_t tag, const char* attribute) {
    for (lxb_dom_node_t* child = node ? node->first_child : NULL; child; child = child->next) {
        size_t length = 0;
        if (child->type == LXB_DOM_NODE_TYPE_ELEMENT && child->local_name == tag) {
            if (!attribute || element_attribute(lxb_dom_interface_element(child), attribute, &length))
                return child;
        }
        lxb_dom_node_t* nested = find_element(child, tag, attribute);
        if (nested)
            return nested;
    }
    return NULL;
}

static bool subtree_has_rendered_h1(HtmlMarkdownContext* context, lxb_dom_node_t* node) {
    if (!node)
        return false;
    if (node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        lxb_dom_element_t* element = lxb_dom_interface_element(node);
        if (element_is_hidden(element) || element_has_chrome_role(element) || element_has_ignored_tag(node) ||
            bx_fetch_site_markdown_skip_node(context->site, node) || (context->lore_kernel_org && bx_fetch_lore_markdown_skip_node(node))) {
            return false;
        }
        if (node->local_name == LXB_TAG_H1 && node_has_text(node))
            return true;
    }
    for (lxb_dom_node_t* child = node->first_child; child; child = child->next) {
        if (subtree_has_rendered_h1(context, child))
            return true;
    }
    return false;
}

int bx_fetch_html_markdown_supported(void) {
    return 1;
}

char* bx_fetch_html_to_markdown(const char* base_url, const char* html_data, size_t len, bool absolute_links, size_t* output_len) {
    if (output_len)
        *output_len = 0;
    if (!html_data) {
        errno = EINVAL;
        return NULL;
    }
    if (len > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES) {
        errno = EFBIG;
        return NULL;
    }

    lxb_html_document_t* document = lxb_html_document_create();
    if (!document)
        return NULL;
    if (lxb_html_document_parse(document, (const lxb_char_t*)html_data, len) != LXB_STATUS_OK) {
        lxb_html_document_destroy(document);
        errno = EINVAL;
        return NULL;
    }

    BxMarkdownWriter output;
    bx_markdown_writer_init(&output, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    HtmlMarkdownContext context = {
        .output = &output,
        .link_base = absolute_links ? base_url : NULL,
        .lore_kernel_org = bx_fetch_lore_markdown_url_matches(base_url),
        .site = bx_fetch_markdown_site_for_url(base_url),
    };
    char* document_base = NULL;
    if (absolute_links && base_url) {
        lxb_html_head_element_t* head = lxb_html_document_head_element(document);
        lxb_dom_node_t* base = find_element(head ? lxb_dom_interface_node(head) : NULL, LXB_TAG_BASE, "href");
        size_t length = 0;
        const lxb_char_t* href = base ? element_attribute(lxb_dom_interface_element(base), "href", &length) : NULL;
        if (href) {
            char* reference = strndup((const char*)href, length);
            if (!reference) {
                lxb_html_document_destroy(document);
                return NULL;
            }
            errno = 0;
            document_base = bx_fetch_url_resolve(base_url, reference);
            int error_number = errno;
            free(reference);
            if (!document_base && error_number == ENOMEM) {
                lxb_html_document_destroy(document);
                return NULL;
            }
            if (document_base)
                context.link_base = document_base;
        }
    }
    lxb_html_body_element_t* body = lxb_html_document_body_element(document);
    lxb_dom_node_t* body_node = body ? lxb_dom_interface_node(body) : lxb_dom_interface_node(document);
    bool rendered = true;
    if (!context.lore_kernel_org && !subtree_has_rendered_h1(&context, body_node)) {
        lxb_html_head_element_t* head = lxb_html_document_head_element(document);
        lxb_dom_node_t* title = find_element(head ? lxb_dom_interface_node(head) : NULL, LXB_TAG_TITLE, NULL);
        if (title && node_has_text(title))
            rendered = render_heading(&context, title, 1u);
    }
    rendered = rendered && render_children(&context, body_node);
    char* result = rendered ? bx_markdown_writer_take(&output, output_len) : NULL;
    int error_number = errno;
    bx_markdown_writer_clear(&output);
    free(document_base);
    lxb_html_document_destroy(document);
    if (!result)
        errno = error_number ? error_number : EINVAL;
    return result;
}

#else

int bx_fetch_html_markdown_supported(void) {
    return 0;
}

char* bx_fetch_html_to_markdown(const char* base_url, const char* html_data, size_t len, bool absolute_links, size_t* output_len) {
    (void)base_url;
    (void)absolute_links;
    (void)html_data;
    (void)len;
    if (output_len)
        *output_len = 0;
    errno = ENOTSUP;
    return NULL;
}

#endif
