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

#include <liblexa/html.h>

/* Private native candidate. Production selection still requires the bx
 * consumer matrix and the complete HTML parser grammar. */
typedef struct {
    lxa_dom_nodes_t* nodes;
    BxMarkdownWriter* output;
    const char* link_base;
    BxFetchMarkdownSite site;
    bool lore;
    size_t list_depth;
    size_t ordered_index[32];
    bool ordered[32];
    bool in_list_item;
} NativeMarkdown;

static bool native_markdown_status(lxa_status_t status) {
    if (status == LXA_OK)
        return true;
    errno = status == LXA_ERROR_NO_MEMORY ? ENOMEM :
            status == LXA_ERROR_LIMIT || status == LXA_ERROR_OVERFLOW ? EFBIG :
            status == LXA_ERROR_ENTROPY ? EIO : EINVAL;
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

static bool native_markdown_ignored(lxa_span_t name) {
    static const char* const names[] = {
        "head", "script", "style", "template", "noscript", "svg", "canvas",
        "iframe", "nav", "aside", "footer", "form", "button", "input",
        "select", "textarea"
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        if (native_markdown_name(name, names[i]))
            return true;
    }
    return false;
}

static bool native_markdown_heading(NativeMarkdown* context, lxa_dom_ref_t node,
                                    size_t depth, size_t level) {
    bool present;
    if (!native_markdown_has_text(context, node, &present))
        return false;
    if (!present)
        return true;
    static const char hashes[] = "######";
    return bx_markdown_writer_newlines(context->output, 2u)
        && bx_markdown_writer_raw(context->output, hashes, level)
        && bx_markdown_writer_raw(context->output, " ", 1u)
        && native_markdown_children(context, node, depth + 1)
        && bx_markdown_writer_newlines(context->output, 2u);
}

static bool native_markdown_block(NativeMarkdown* context, lxa_dom_ref_t node,
                                  size_t depth) {
    if (context->in_list_item)
        return native_markdown_children(context, node, depth + 1)
            && bx_markdown_writer_newlines(context->output, 1u);
    return bx_markdown_writer_newlines(context->output, 2u)
        && native_markdown_children(context, node, depth + 1)
        && bx_markdown_writer_newlines(context->output, 2u);
}

static bool native_markdown_has_h1(NativeMarkdown* context, lxa_dom_ref_t node,
                                   size_t depth, bool* found) {
    if (depth > 256) {
        errno = EFBIG;
        return false;
    }
    lxa_dom_record_t record;
    if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, node, &record)))
        return false;
    if (record.kind == LXA_DOM_KIND_ELEMENT) {
        bool skip = false;
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
        lxa_span_t name;
        if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, node, &name)))
            return false;
        if (native_markdown_ignored(name))
            return true;
        if (!native_markdown_hidden(context, node, &skip))
            return false;
        if (skip)
            return true;
        if (native_markdown_name(name, "h1")
            && !native_markdown_has_text(context, node, found))
            return false;
        if (*found)
            return true;
    }
    if (record.kind != LXA_DOM_KIND_ELEMENT && record.kind != LXA_DOM_KIND_DOCUMENT)
        return true;
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, node, &child)))
        return false;
    while (child.handle && !*found) {
        if (!native_markdown_has_h1(context, child, depth + 1, found))
            return false;
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    return true;
}

static bool native_markdown_find_head_element(NativeMarkdown* context,
                                               lxa_dom_ref_t head,
                                               const char* tag, const char* attribute,
                                               lxa_dom_ref_t* found) {
    *found = (lxa_dom_ref_t){0};
    if (!head.handle)
        return true;
    lxa_dom_ref_t node;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, head, &node)))
        return false;
    while (node.handle) {
        lxa_dom_record_t record;
        if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, node, &record)))
            return false;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            lxa_span_t name;
            if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, node, &name)))
                return false;
            if (native_markdown_name(name, tag)) {
                if (!attribute) {
                    *found = node;
                    break;
                }
                lxa_span_t key = {(const uint8_t*)attribute, strlen(attribute)};
                lxa_dom_ref_t attr;
                if (!native_markdown_status(lxa_dom_nodes_find_attribute(
                    context->nodes, node, key, &attr)))
                    return false;
                if (attr.handle) {
                    *found = node;
                    break;
                }
            }
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, node, &next)))
            return false;
        node = next;
    }
    return true;
}

static bool native_markdown_destination(NativeMarkdown* context, lxa_span_t value) {
    char* resolved = NULL;
    if (context->link_base) {
        char* reference = strndup(value.data ? (const char*)value.data : "", value.length);
        if (!reference) {
            errno = ENOMEM;
            return false;
        }
        errno = 0;
        if (!bx_fetch_url_has_explicit_scheme(reference))
            resolved = bx_fetch_url_resolve(context->link_base, reference);
        int error_number = errno;
        free(reference);
        if (!resolved && error_number == ENOMEM)
            return false;
    }
    if (resolved)
        value = (lxa_span_t){(const uint8_t*)resolved, strlen(resolved)};
    bool angle = false;
    for (size_t i = 0; i < value.length; i++) {
        if (isspace((unsigned char)value.data[i]) || value.data[i] == '(' || value.data[i] == ')') {
            angle = true;
            break;
        }
    }
    bool ok = !angle || bx_markdown_writer_raw(context->output, "<", 1u);
    for (size_t i = 0; ok && i < value.length; i++) {
        char c = (char)value.data[i];
        if ((c == '\\' || (!angle && c == ')') || (angle && c == '>'))
            && !(ok = bx_markdown_writer_raw(context->output, "\\", 1u)))
            break;
        ok = bx_markdown_writer_raw(context->output, &c, 1u);
    }
    if (ok && angle)
        ok = bx_markdown_writer_raw(context->output, ">", 1u);
    free(resolved);
    return ok;
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
            && native_markdown_destination(context, href)
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
        && native_markdown_destination(context, src)
        && bx_markdown_writer_raw(context->output, ")", 1u);
}

static bool native_markdown_list(NativeMarkdown* context, lxa_dom_ref_t node,
                                 size_t depth, bool ordered) {
    if (context->list_depth >= sizeof(context->ordered) / sizeof(*context->ordered)) {
        errno = EFBIG;
        return false;
    }
    if (!bx_markdown_writer_newlines(context->output, context->list_depth ? 1u : 2u))
        return false;
    size_t index = context->list_depth++;
    context->ordered[index] = ordered;
    context->ordered_index[index] = 1u;
    if (ordered) {
        lxa_span_t start;
        if (!native_markdown_attribute(context, node, "start", &start)) {
            context->list_depth--;
            return false;
        }
        size_t parsed = 0;
        bool valid = start.data && start.length;
        for (size_t i = 0; valid && i < start.length; i++) {
            uint8_t digit = start.data[i];
            if (!isdigit((unsigned char)digit) || parsed > (SIZE_MAX - (size_t)(digit - '0')) / 10u)
                valid = false;
            else
                parsed = parsed * 10u + (size_t)(digit - '0');
        }
        if (valid)
            context->ordered_index[index] = parsed;
    }
    bool rendered = native_markdown_children(context, node, depth + 1);
    context->list_depth--;
    return rendered && bx_markdown_writer_newlines(context->output,
                                                   context->list_depth ? 1u : 2u);
}

static bool native_markdown_list_item(NativeMarkdown* context, lxa_dom_ref_t node,
                                      size_t depth) {
    if (!context->list_depth)
        return native_markdown_block(context, node, depth);
    size_t index = context->list_depth - 1u;
    if (!bx_markdown_writer_newlines(context->output, 1u))
        return false;
    for (size_t i = 0; i < index; i++) {
        if (!bx_markdown_writer_raw(context->output, "  ", 2u))
            return false;
    }
    if (context->ordered[index]) {
        char prefix[32];
        int length = snprintf(prefix, sizeof(prefix), "%zu. ",
                              context->ordered_index[index]++);
        if (length < 0 || (size_t)length >= sizeof(prefix)
            || !bx_markdown_writer_raw(context->output, prefix, (size_t)length))
            return false;
    }
    else if (!bx_markdown_writer_raw(context->output, "- ", 2u))
        return false;
    bool was_in_list_item = context->in_list_item;
    context->in_list_item = true;
    bool rendered = native_markdown_children(context, node, depth + 1);
    context->in_list_item = was_in_list_item;
    return rendered;
}

static bool native_markdown_blockquote(NativeMarkdown* context, lxa_dom_ref_t node,
                                        size_t depth) {
    BxMarkdownWriter nested;
    bx_markdown_writer_init(&nested, BX_FETCH_DOCUMENT_PARSE_MAX_BYTES);
    NativeMarkdown nested_context = *context;
    nested_context.output = &nested;
    bool rendered = native_markdown_children(&nested_context, node, depth + 1);
    size_t length = 0;
    char* text = rendered ? bx_markdown_writer_take(&nested, &length) : NULL;
    int failure = errno;
    bx_markdown_writer_clear(&nested);
    if (!text) {
        errno = failure ? failure : EINVAL;
        return false;
    }
    bool ok = bx_markdown_writer_newlines(context->output, 2u);
    for (size_t start = 0; ok && start < length;) {
        size_t end = start;
        while (end < length && text[end] != '\n') end++;
        ok = bx_markdown_writer_raw(context->output, "> ", 2u)
            && bx_markdown_writer_raw(context->output, text + start, end - start)
            && bx_markdown_writer_newlines(context->output, 1u);
        start = end < length ? end + 1u : end;
    }
    free(text);
    return ok && bx_markdown_writer_newlines(context->output, 2u);
}

static bool native_markdown_element_is(NativeMarkdown* context, lxa_dom_ref_t node,
                                       const char* tag, bool* match) {
    lxa_dom_record_t record;
    if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, node, &record)))
        return false;
    *match = false;
    if (record.kind != LXA_DOM_KIND_ELEMENT)
        return true;
    lxa_span_t name;
    if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, node, &name)))
        return false;
    *match = native_markdown_name(name, tag);
    return true;
}

static bool native_markdown_visible_cell(NativeMarkdown* context, lxa_dom_ref_t node,
                                         bool* visible) {
    bool th, td, hidden, skip;
    if (!native_markdown_element_is(context, node, "th", &th)
        || !native_markdown_element_is(context, node, "td", &td))
        return false;
    *visible = false;
    if (!th && !td)
        return true;
    if (!native_markdown_hidden(context, node, &hidden)
        || !native_markdown_status(bx_fetch_site_markdown_skip_node(
            context->site, context->nodes, node, &skip)))
        return false;
    *visible = !hidden && !skip;
    return true;
}

static bool native_markdown_cell_span(NativeMarkdown* context, lxa_dom_ref_t node,
                                      size_t* span) {
    lxa_span_t value;
    if (!native_markdown_attribute(context, node, "colspan", &value))
        return false;
    *span = 1;
    if (!value.data || !value.length)
        return true;
    size_t parsed = 0;
    for (size_t i = 0; i < value.length; i++) {
        uint8_t digit = value.data[i];
        if (!isdigit((unsigned char)digit)
            || parsed > (SIZE_MAX - (size_t)(digit - '0')) / 10u)
            return true;
        parsed = parsed * 10u + (size_t)(digit - '0');
    }
    if (parsed && parsed <= 1000u)
        *span = parsed;
    return true;
}

static bool native_markdown_table_cell_text(NativeMarkdown* context,
                                             lxa_dom_ref_t node, size_t depth) {
    if (depth > 256) {
        errno = EFBIG;
        return false;
    }
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
    bool hidden, skip;
    if (!native_markdown_hidden(context, node, &hidden)
        || !native_markdown_status(bx_fetch_site_markdown_skip_node(
            context->site, context->nodes, node, &skip)))
        return false;
    if (hidden || skip || native_markdown_ignored(name)
        || native_markdown_name(name, "img") || native_markdown_name(name, "image"))
        return true;
    if (native_markdown_name(name, "br")
        && !bx_markdown_writer_text(context->output, " ", 1u))
        return false;
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, node, &child)))
        return false;
    while (child.handle) {
        if (!native_markdown_table_cell_text(context, child, depth + 1))
            return false;
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    return true;
}

static bool native_markdown_row_columns(NativeMarkdown* context, lxa_dom_ref_t row,
                                        size_t* count) {
    *count = 0;
    lxa_dom_ref_t cell;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, row, &cell)))
        return false;
    while (cell.handle) {
        bool visible;
        if (!native_markdown_visible_cell(context, cell, &visible))
            return false;
        if (visible) {
            size_t span;
            if (!native_markdown_cell_span(context, cell, &span))
                return false;
            if (span > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES - *count) {
                errno = EFBIG;
                return false;
            }
            *count += span;
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, cell, &next)))
            return false;
        cell = next;
    }
    return true;
}

static bool native_markdown_table_columns(NativeMarkdown* context,
                                           lxa_dom_ref_t node, size_t depth,
                                           size_t* columns) {
    if (depth > 256) {
        errno = EFBIG;
        return false;
    }
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, node, &child)))
        return false;
    while (child.handle) {
        lxa_dom_record_t record;
        if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, child, &record)))
            return false;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            bool hidden, skip, row, section;
            lxa_span_t name;
            if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, child, &name))
                || !native_markdown_hidden(context, child, &hidden)
                || !native_markdown_status(bx_fetch_site_markdown_skip_node(
                    context->site, context->nodes, child, &skip)))
                return false;
            if (!hidden && !skip) {
                row = native_markdown_name(name, "tr");
                section = native_markdown_name(name, "thead")
                    || native_markdown_name(name, "tbody") || native_markdown_name(name, "tfoot");
                size_t count = 0;
                if ((row && !native_markdown_row_columns(context, child, &count))
                    || (section && !native_markdown_table_columns(
                        context, child, depth + 1, &count)))
                    return false;
                if (count > *columns)
                    *columns = count;
            }
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    return true;
}

static bool native_markdown_table_row(NativeMarkdown* context, lxa_dom_ref_t row,
                                      size_t columns, bool first_row) {
    size_t count;
    if (!native_markdown_row_columns(context, row, &count))
        return false;
    if (!count)
        return true;
    if (!bx_markdown_writer_raw(context->output, "|", 1u))
        return false;
    lxa_dom_ref_t cell;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, row, &cell)))
        return false;
    while (cell.handle) {
        bool visible;
        if (!native_markdown_visible_cell(context, cell, &visible))
            return false;
        if (visible) {
            size_t span;
            if (!native_markdown_cell_span(context, cell, &span)
                || !bx_markdown_writer_raw(context->output, " ", 1u)
                || !native_markdown_table_cell_text(context, cell, 0)
                || !bx_markdown_writer_raw(context->output, " |", 2u))
                return false;
            for (size_t i = 1; i < span; i++) {
                if (!bx_markdown_writer_raw(context->output, "  |", 3u))
                    return false;
            }
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, cell, &next)))
            return false;
        cell = next;
    }
    for (size_t i = count; i < columns; i++) {
        if (!bx_markdown_writer_raw(context->output, "  |", 3u))
            return false;
    }
    if (!bx_markdown_writer_newlines(context->output, 1u))
        return false;
    if (!first_row)
        return true;
    for (size_t i = 0; i < columns; i++) {
        if (!bx_markdown_writer_raw(context->output, "| --- ", 6u))
            return false;
    }
    return bx_markdown_writer_raw(context->output, "|", 1u)
        && bx_markdown_writer_newlines(context->output, 1u);
}

static bool native_markdown_table_rows(NativeMarkdown* context, lxa_dom_ref_t node,
                                       size_t depth, size_t columns, bool* first) {
    if (depth > 256) {
        errno = EFBIG;
        return false;
    }
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, node, &child)))
        return false;
    while (child.handle) {
        lxa_dom_record_t record;
        if (!native_markdown_status(lxa_dom_nodes_read(context->nodes, child, &record)))
            return false;
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            lxa_span_t name;
            bool hidden, row, section;
            if (!native_markdown_status(lxa_dom_nodes_element_name(context->nodes, child, &name))
                || !native_markdown_hidden(context, child, &hidden))
                return false;
            if (!hidden) {
                row = native_markdown_name(name, "tr");
                section = native_markdown_name(name, "thead")
                    || native_markdown_name(name, "tbody") || native_markdown_name(name, "tfoot");
                if (row) {
                    size_t count;
                    if (!native_markdown_row_columns(context, child, &count)
                        || !native_markdown_table_row(context, child, columns, *first))
                        return false;
                    if (count)
                        *first = false;
                }
                else if (section && !native_markdown_table_rows(
                    context, child, depth + 1, columns, first))
                    return false;
            }
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    return true;
}

static bool native_markdown_table(NativeMarkdown* context, lxa_dom_ref_t table,
                                  size_t depth) {
    size_t columns = 0;
    if (!native_markdown_table_columns(context, table, depth, &columns))
        return false;
    if (!columns)
        return true;
    if (!bx_markdown_writer_newlines(context->output, 2u))
        return false;
    lxa_dom_ref_t child;
    if (!native_markdown_status(lxa_dom_nodes_first_child(context->nodes, table, &child)))
        return false;
    while (child.handle) {
        bool caption, hidden;
        if (!native_markdown_element_is(context, child, "caption", &caption))
            return false;
        if (caption) {
            if (!native_markdown_hidden(context, child, &hidden))
                return false;
            if (!hidden && (!native_markdown_wrapped(context, child, depth, "**")
                            || !bx_markdown_writer_newlines(context->output, 1u)))
                return false;
        }
        lxa_dom_ref_t next;
        if (!native_markdown_status(lxa_dom_nodes_next_sibling(context->nodes, child, &next)))
            return false;
        child = next;
    }
    bool first = true;
    return native_markdown_table_rows(context, table, depth, columns, &first)
        && bx_markdown_writer_newlines(context->output, 2u);
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
    if (native_markdown_ignored(name))
        return true;
    bool hidden;
    if (!native_markdown_hidden(context, node, &hidden))
        return false;
    if (hidden)
        return true;
    if (unwrap || native_markdown_name(name, "span") || native_markdown_name(name, "body"))
        return native_markdown_children(context, node, depth + 1);
    if (name.length == 2 && name.data[0] == 'h' && name.data[1] >= '1' && name.data[1] <= '6') {
        return native_markdown_heading(context, node, depth, (size_t)(name.data[1] - '0'));
    }
    if (native_markdown_name(name, "p") || native_markdown_name(name, "div")
        || native_markdown_name(name, "section") || native_markdown_name(name, "article")
        || native_markdown_name(name, "main") || native_markdown_name(name, "header")
        || native_markdown_name(name, "figure") || native_markdown_name(name, "figcaption")
        || native_markdown_name(name, "details") || native_markdown_name(name, "summary")) {
        return native_markdown_block(context, node, depth);
    }
    if (native_markdown_name(name, "br"))
        return bx_markdown_writer_raw(context->output, "\\\n", 2u);
    if (native_markdown_name(name, "hr"))
        return bx_markdown_writer_newlines(context->output, 2u)
            && bx_markdown_writer_raw(context->output, "---", 3u)
            && bx_markdown_writer_newlines(context->output, 2u);
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
    if (native_markdown_name(name, "ul") || native_markdown_name(name, "ol"))
        return native_markdown_list(context, node, depth, native_markdown_name(name, "ol"));
    if (native_markdown_name(name, "li"))
        return native_markdown_list_item(context, node, depth);
    if (native_markdown_name(name, "dl"))
        return native_markdown_block(context, node, depth);
    if (native_markdown_name(name, "dt"))
        return bx_markdown_writer_newlines(context->output, 1u)
            && native_markdown_wrapped(context, node, depth, "**");
    if (native_markdown_name(name, "dd"))
        return bx_markdown_writer_newlines(context->output, 1u)
            && bx_markdown_writer_raw(context->output, ": ", 2u)
            && native_markdown_children(context, node, depth + 1);
    if (native_markdown_name(name, "blockquote"))
        return native_markdown_blockquote(context, node, depth);
    if (native_markdown_name(name, "table"))
        return native_markdown_table(context, node, depth);
    return native_markdown_children(context, node, depth + 1);
}

int bx_fetch_html_markdown_supported(void) {
    return 1;
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
        .link_base = absolute_links ? base_url : NULL,
        .site = bx_fetch_markdown_site_for_url(base_url),
        .lore = bx_fetch_lore_markdown_url_matches(base_url),
    };
    char* document_base = NULL;
    bool ready = true;
    if (absolute_links && base_url) {
        lxa_dom_ref_t base;
        ready = native_markdown_find_head_element(&context,
            lxa_html_document_head(document), "base", "href", &base);
        if (ready && base.handle) {
            lxa_span_t href;
            ready = native_markdown_attribute(&context, base, "href", &href);
            if (ready) {
                char* reference = strndup(href.data ? (const char*)href.data : "", href.length);
                if (!reference) {
                    errno = ENOMEM;
                    ready = false;
                }
                else {
                    errno = 0;
                    document_base = bx_fetch_url_resolve(base_url, reference);
                    int failure = errno;
                    free(reference);
                    if (!document_base && failure == ENOMEM)
                        ready = false;
                    if (document_base)
                        context.link_base = document_base;
                }
            }
        }
    }
    lxa_dom_ref_t body = lxa_html_document_body(document);
    bool has_h1 = false;
    bool rendered = ready && body.handle
        && native_markdown_has_h1(&context, body, 0, &has_h1);
    if (rendered && !has_h1 && !context.lore) {
        lxa_dom_ref_t title;
        rendered = native_markdown_find_head_element(&context,
            lxa_html_document_head(document), "title", NULL, &title)
            && (!title.handle || native_markdown_heading(&context, title, 0, 1u));
    }
    rendered = rendered && native_markdown_children(&context, body, 0);
    char* result = rendered ? bx_markdown_writer_take(&output, output_len) : NULL;
    int failure = errno;
    bx_markdown_writer_clear(&output);
    free(document_base);
    lxa_html_document_destroy(document);
    if (!result) errno = failure ? failure : EINVAL;
    return result;
}
