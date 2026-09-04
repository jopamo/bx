#define _GNU_SOURCE
#include "lib/fetch/html.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static bool document_parser_input_valid(const char* data, size_t len) {
    if (!data) {
        errno = EINVAL;
        return false;
    }
    if (len > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES) {
        errno = EFBIG;
        return false;
    }
    return true;
}

static bool is_css_identifier_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_';
}

static bool has_ascii_case_prefix(const char* text, size_t len, size_t i, const char* prefix) {
    size_t plen = strlen(prefix);
    if (!text || !prefix || (i + plen) > len) {
        return false;
    }

    for (size_t j = 0; j < plen; j++) {
        if (tolower((unsigned char)text[i + j]) != tolower((unsigned char)prefix[j])) {
            return false;
        }
    }
    return true;
}

static size_t skip_css_comment(const char* css, size_t len, size_t i) {
    if ((i + 1) >= len || css[i] != '/' || css[i + 1] != '*') {
        return i;
    }

    i += 2;
    while ((i + 1) < len) {
        if (css[i] == '*' && css[i + 1] == '/') {
            return i + 2;
        }
        i++;
    }
    return len;
}

static size_t skip_css_quoted_string(const char* css, size_t len, size_t i) {
    if (i >= len || (css[i] != '\'' && css[i] != '"')) {
        return i;
    }

    char quote = css[i++];
    bool escaped = false;
    while (i < len) {
        char c = css[i];
        if (escaped) {
            escaped = false;
            i++;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            i++;
            continue;
        }
        if (c == quote) {
            i++;
            break;
        }
        i++;
    }
    return i;
}

static size_t skip_css_whitespace_and_comments(const char* css, size_t len, size_t i) {
    while (i < len) {
        if (isspace((unsigned char)css[i])) {
            i++;
            continue;
        }
        if ((i + 1) < len && css[i] == '/' && css[i + 1] == '*') {
            i = skip_css_comment(css, len, i);
            continue;
        }
        break;
    }
    return i;
}

static bool append_decoded_byte(char** buf, size_t* used, size_t* cap, unsigned char byte) {
    if (!buf || !used || !cap)
        return false;

    if ((*used + 1) >= *cap) {
        size_t next_cap = (*cap == 0) ? 32 : (*cap * 2);
        char* grown = realloc(*buf, next_cap);
        if (!grown)
            return false;
        *buf = grown;
        *cap = next_cap;
    }

    (*buf)[(*used)++] = (char)byte;
    return true;
}

static bool append_decoded_codepoint(char** buf, size_t* used, size_t* cap, unsigned int cp) {
    if (cp <= 0x7F) {
        return append_decoded_byte(buf, used, cap, (unsigned char)cp);
    }
    if (cp <= 0x7FF) {
        return append_decoded_byte(buf, used, cap, (unsigned char)(0xC0 | (cp >> 6))) && append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0xFFFF) {
        return append_decoded_byte(buf, used, cap, (unsigned char)(0xE0 | (cp >> 12))) && append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | ((cp >> 6) & 0x3F))) &&
               append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0x10FFFF) {
        return append_decoded_byte(buf, used, cap, (unsigned char)(0xF0 | (cp >> 18))) && append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | ((cp >> 12) & 0x3F))) &&
               append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | ((cp >> 6) & 0x3F))) && append_decoded_byte(buf, used, cap, (unsigned char)(0x80 | (cp & 0x3F)));
    }

    return append_decoded_codepoint(buf, used, cap, 0xFFFD);
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static bool append_css_decoded_range(const char* start, size_t len, char** buf, size_t* used, size_t* cap) {
    if (!start)
        return false;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)start[i];
        if (c != '\\') {
            if (!append_decoded_byte(buf, used, cap, c)) {
                return false;
            }
            continue;
        }

        if ((i + 1) >= len) {
            break;
        }

        i++;
        unsigned char next = (unsigned char)start[i];
        if (next == '\r') {
            if ((i + 1) < len && start[i + 1] == '\n') {
                i++;
            }
            continue;
        }
        if (next == '\n' || next == '\f') {
            continue;
        }

        int hv = hex_value(next);
        if (hv >= 0) {
            unsigned int codepoint = (unsigned int)hv;
            int digits = 1;
            while ((i + 1) < len && digits < 6) {
                int extra = hex_value((unsigned char)start[i + 1]);
                if (extra < 0)
                    break;
                codepoint = (codepoint * 16u) + (unsigned int)extra;
                i++;
                digits++;
            }

            if ((i + 1) < len && isspace((unsigned char)start[i + 1])) {
                i++;
                if (start[i] == '\r' && (i + 1) < len && start[i + 1] == '\n') {
                    i++;
                }
            }

            if (codepoint == 0 || codepoint > 0x10FFFF) {
                codepoint = 0xFFFD;
            }
            if (!append_decoded_codepoint(buf, used, cap, codepoint)) {
                return false;
            }
            continue;
        }

        if (!append_decoded_byte(buf, used, cap, next)) {
            return false;
        }
    }

    return true;
}

static void emit_css_url(const char* start, size_t len, BxFetchLinkCallback cb, void* userdata) {
    if (!start || !cb)
        return;

    while (len > 0 && isspace((unsigned char)*start)) {
        start++;
        len--;
    }
    while (len > 0 && isspace((unsigned char)start[len - 1])) {
        len--;
    }
    if (len == 0)
        return;

    char* url = NULL;
    size_t used = 0;
    size_t cap = 0;
    if (!append_css_decoded_range(start, len, &url, &used, &cap)) {
        free(url);
        return;
    }
    if (!append_decoded_byte(&url, &used, &cap, '\0')) {
        free(url);
        return;
    }
    if (!url)
        return;
    cb(userdata, url);
    free(url);
}

static size_t scan_css_value_until(const char* css, size_t len, size_t i, char terminator, bool* found_terminator) {
    bool escaped = false;
    while (i < len) {
        char c = css[i];
        if (escaped) {
            escaped = false;
            i++;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            i++;
            continue;
        }
        if (c == terminator) {
            break;
        }
        i++;
    }

    if (found_terminator) {
        *found_terminator = (i < len && css[i] == terminator);
    }
    return i;
}

static size_t parse_css_quoted_value(const char* css, size_t len, size_t i, char quote, BxFetchLinkCallback cb, void* userdata) {
    size_t value_start = i;
    bool closed_quote = false;
    i = scan_css_value_until(css, len, i, quote, &closed_quote);
    size_t value_end = i;
    if (closed_quote) {
        i++;
    }

    while (i < len && isspace((unsigned char)css[i])) {
        i++;
    }
    while (i < len && css[i] != ')') {
        i++;
    }
    if (i < len && css[i] == ')') {
        i++;
    }

    emit_css_url(css + value_start, value_end - value_start, cb, userdata);
    return i;
}

static size_t parse_css_unquoted_value(const char* css, size_t len, size_t i, BxFetchLinkCallback cb, void* userdata) {
    size_t value_start = i;
    bool found_closing_paren = false;
    i = scan_css_value_until(css, len, i, ')', &found_closing_paren);

    emit_css_url(css + value_start, i - value_start, cb, userdata);

    if (found_closing_paren) {
        i++;
    }
    return i;
}

static size_t parse_css_url_function(const char* css, size_t len, size_t i, BxFetchLinkCallback cb, void* userdata) {
    while (i < len && isspace((unsigned char)css[i])) {
        i++;
    }
    if (i >= len)
        return i;

    if (css[i] == '\'' || css[i] == '"') {
        char quote = css[i];
        return parse_css_quoted_value(css, len, i + 1, quote, cb, userdata);
    }

    return parse_css_unquoted_value(css, len, i, cb, userdata);
}

static size_t parse_css_import_rule(const char* css, size_t len, size_t i, BxFetchLinkCallback cb, void* userdata) {
    if (i >= len || css[i] != '@') {
        return i;
    }

    size_t import_start = i + 1;
    if (!has_ascii_case_prefix(css, len, import_start, "import")) {
        return i + 1;
    }
    if ((import_start + 6) < len && is_css_identifier_char((unsigned char)css[import_start + 6])) {
        return i + 1;
    }

    size_t j = skip_css_whitespace_and_comments(css, len, import_start + 6);
    if (j < len && (css[j] == '\'' || css[j] == '"')) {
        char quote = css[j];
        size_t value_start = j + 1;
        j = skip_css_quoted_string(css, len, j);
        if (j > value_start && j <= len && css[j - 1] == quote) {
            emit_css_url(css + value_start, j - value_start - 1, cb, userdata);
        }
    }
    else if (has_ascii_case_prefix(css, len, j, "url")) {
        size_t k = j + 3;
        while (k < len && isspace((unsigned char)css[k])) {
            k++;
        }
        if (k < len && css[k] == '(') {
            j = parse_css_url_function(css, len, k + 1, cb, userdata);
        }
    }

    while (j < len) {
        if ((j + 1) < len && css[j] == '/' && css[j + 1] == '*') {
            j = skip_css_comment(css, len, j);
            continue;
        }
        if (css[j] == '\'' || css[j] == '"') {
            j = skip_css_quoted_string(css, len, j);
            continue;
        }
        if (css[j] == ';') {
            j++;
            break;
        }
        j++;
    }
    return j;
}

int bx_fetch_css_extract_links(const char* base_url, const char* css_data, size_t len, BxFetchLinkCallback cb, void* userdata) {
    (void)base_url;
    if (!cb)
        errno = EINVAL;
    if (!cb || !document_parser_input_valid(css_data, len))
        return -1;

    size_t i = 0;
    while (i < len) {
        if (css_data[i] == '/' && (i + 1) < len && css_data[i + 1] == '*') {
            i = skip_css_comment(css_data, len, i);
            continue;
        }

        if (css_data[i] == '@') {
            i = parse_css_import_rule(css_data, len, i, cb, userdata);
            continue;
        }

        if (css_data[i] == '\'' || css_data[i] == '"') {
            i = skip_css_quoted_string(css_data, len, i);
            continue;
        }

        bool maybe_url = (i + 2) < len && (css_data[i] == 'u' || css_data[i] == 'U') && (css_data[i + 1] == 'r' || css_data[i + 1] == 'R') && (css_data[i + 2] == 'l' || css_data[i + 2] == 'L');
        if (!maybe_url) {
            i++;
            continue;
        }

        if (i > 0 && is_css_identifier_char((unsigned char)css_data[i - 1])) {
            i++;
            continue;
        }

        size_t j = i + 3;
        while (j < len && isspace((unsigned char)css_data[j])) {
            j++;
        }
        if (j >= len || css_data[j] != '(') {
            i++;
            continue;
        }

        i = parse_css_url_function(css_data, len, j + 1, cb, userdata);
    }

    return 0;
}

#if HAVE_LEXBOR
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/html/html.h>
#include <lexbor/html/serialize.h>

typedef struct {
    BxFetchHtmlLinkCallback cb;
    void* userdata;
} LexborExtractContext;

typedef struct {
    BxFetchLinkRewriteCallback cb;
    void* userdata;
} LexborRewriteContext;

typedef void (*LexborAttrVisitor)(lxb_dom_element_t* element, const char* attr_name, size_t attr_name_len, const char* url, void* userdata);

typedef struct {
    const char* name;
    size_t len;
} LexborAttrSpec;

typedef struct {
    BxFetchLinkCallback cb;
    void* userdata;
} HtmlExtractCompatContext;

static void extract_html_link_compat(void* userdata, const char* url, BxFetchHtmlLinkKind kind) {
    (void)kind;

    const HtmlExtractCompatContext* ctx = userdata;
    if (!ctx || !ctx->cb)
        return;
    ctx->cb(ctx->userdata, url);
}

static bool span_ascii_case_equals_bytes(const char* value, size_t len, const char* expected) {
    if (!value || !expected || len != strlen(expected))
        return false;

    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)value[i]) != tolower((unsigned char)expected[i])) {
            return false;
        }
    }

    return true;
}

static BxFetchHtmlLinkKind lexbor_html_link_kind(lxb_dom_element_t* element, const char* attr_name, size_t attr_name_len) {
    if (!span_ascii_case_equals_bytes(attr_name, attr_name_len, "href")) {
        return BX_FETCH_HTML_LINK_REQUISITE;
    }

    size_t tag_name_len = 0;
    const lxb_char_t* tag_name = lxb_dom_element_tag_name(element, &tag_name_len);
    if (tag_name && span_ascii_case_equals_bytes((const char*)tag_name, tag_name_len, "a")) {
        return BX_FETCH_HTML_LINK_NAVIGATION;
    }

    return BX_FETCH_HTML_LINK_REQUISITE;
}

static void visit_lexbor_link_attributes(lxb_dom_element_t* element, LexborAttrVisitor visitor, void* visitor_userdata) {
    if (!element || !visitor)
        return;

    static const LexborAttrSpec attrs[] = {
        {"href", 4},
        {"src", 3},
        {NULL, 0},
    };

    for (size_t i = 0; attrs[i].name; i++) {
        size_t value_len = 0;
        const lxb_char_t* value = lxb_dom_element_get_attribute(element, (const lxb_char_t*)attrs[i].name, attrs[i].len, &value_len);
        if (!value)
            continue;

        char* url = strndup((const char*)value, value_len);
        if (!url)
            continue;

        visitor(element, attrs[i].name, attrs[i].len, url, visitor_userdata);
        free(url);
    }
}

static void extract_lexbor_attribute_link(lxb_dom_element_t* element, const char* attr_name, size_t attr_name_len, const char* url, void* userdata) {
    (void)element;

    const LexborExtractContext* extract_ctx = userdata;
    if (!extract_ctx || !extract_ctx->cb)
        return;
    extract_ctx->cb(extract_ctx->userdata, url, lexbor_html_link_kind(element, attr_name, attr_name_len));
}

static void rewrite_lexbor_attribute_link(lxb_dom_element_t* element, const char* attr_name, size_t attr_name_len, const char* url, void* userdata) {
    const LexborRewriteContext* rewrite_ctx = userdata;
    if (!rewrite_ctx || !rewrite_ctx->cb || !element)
        return;

    char* replacement = rewrite_ctx->cb(rewrite_ctx->userdata, url);
    if (!replacement)
        return;

    lxb_dom_element_set_attribute(element, (const lxb_char_t*)attr_name, attr_name_len, (const lxb_char_t*)replacement, strlen(replacement));
    free(replacement);
}

static lxb_dom_report_spec_t callback(lxb_dom_node_t* node, void* ctx) {
    LexborExtractContext* extract_ctx = ctx;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return LXB_DOM_REPORT_OK;

    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    visit_lexbor_link_attributes(element, extract_lexbor_attribute_link, extract_ctx);

    return LXB_DOM_REPORT_OK;
}

static lxb_dom_report_spec_t rewrite_callback(lxb_dom_node_t* node, void* ctx) {
    LexborRewriteContext* rewrite_ctx = ctx;
    if (node->type != LXB_DOM_NODE_TYPE_ELEMENT)
        return LXB_DOM_REPORT_OK;

    lxb_dom_element_t* element = lxb_dom_interface_element(node);
    visit_lexbor_link_attributes(element, rewrite_lexbor_attribute_link, rewrite_ctx);

    return LXB_DOM_REPORT_OK;
}

int bx_fetch_html_extract_links_typed(const char* base_url, const char* html_data, size_t len, BxFetchHtmlLinkCallback cb, void* userdata) {
    (void)base_url;
    if (!cb)
        errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return -1;
    lxb_html_document_t* document = lxb_html_document_create();
    if (!document)
        return -1;

    if (lxb_html_document_parse(document, (const lxb_char_t*)html_data, len) != LXB_STATUS_OK) {
        lxb_html_document_destroy(document);
        return -1;
    }

    LexborExtractContext ctx = {
        .cb = cb,
        .userdata = userdata,
    };
    lxb_dom_node_walk_all_report(lxb_dom_interface_node(document), callback, &ctx);

    lxb_html_document_destroy(document);
    return 0;
}

int bx_fetch_html_extract_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkCallback cb, void* userdata) {
    HtmlExtractCompatContext ctx = {
        .cb = cb,
        .userdata = userdata,
    };
    return bx_fetch_html_extract_links_typed(base_url, html_data, len, extract_html_link_compat, &ctx);
}

char* bx_fetch_html_convert_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkRewriteCallback cb, void* userdata) {
    (void)base_url;
    if (!cb)
        errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return NULL;
    lxb_html_document_t* document = lxb_html_document_create();
    if (!document)
        return NULL;

    if (lxb_html_document_parse(document, (const lxb_char_t*)html_data, len) != LXB_STATUS_OK) {
        lxb_html_document_destroy(document);
        return NULL;
    }

    LexborRewriteContext ctx = {
        .cb = cb,
        .userdata = userdata,
    };
    lxb_dom_node_walk_all_report(lxb_dom_interface_node(document), rewrite_callback, &ctx);

    lexbor_str_t str = {0};
    lxb_html_serialize_tree_str(lxb_dom_interface_node(document), &str);

    char* ret = NULL;
    if (str.data) {
        ret = strdup((const char*)str.data);
        lexbor_str_destroy(&str, lxb_html_document_mraw(document), false);
    }

    lxb_html_document_destroy(document);
    return ret;
}

#else
typedef int (*HtmlAttrVisitorFn)(void* userdata, const char* html_data, size_t tag_name_start, size_t tag_name_end, size_t attr_name_start, size_t attr_name_end, size_t value_start, size_t value_end);

typedef struct {
    BxFetchHtmlLinkCallback cb;
    void* userdata;
} HtmlExtractContext;

typedef struct {
    BxFetchLinkCallback cb;
    void* userdata;
} HtmlExtractCompatContext;

typedef struct {
    size_t start;
    size_t end;
    char* replacement;
} HtmlReplacement;

typedef struct {
    BxFetchLinkRewriteCallback cb;
    void* userdata;
    HtmlReplacement* items;
    size_t count;
    size_t capacity;
} HtmlRewriteContext;

static bool is_html_name_char(unsigned char c) {
    return isalnum(c) || c == '-' || c == '_' || c == ':';
}

static bool span_case_equals(const char* data, size_t start, size_t end, const char* value) {
    if (!data || !value || end < start)
        return false;
    size_t len = end - start;
    if (len != strlen(value))
        return false;

    for (size_t i = 0; i < len; i++) {
        if (tolower((unsigned char)data[start + i]) != tolower((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool span_case_equals_span(const char* data, size_t left_start, size_t left_end, size_t right_start, size_t right_end) {
    if (!data || left_end < left_start || right_end < right_start)
        return false;
    size_t left_len = left_end - left_start;
    size_t right_len = right_end - right_start;
    if (left_len != right_len)
        return false;

    for (size_t i = 0; i < left_len; i++) {
        if (tolower((unsigned char)data[left_start + i]) != tolower((unsigned char)data[right_start + i])) {
            return false;
        }
    }
    return true;
}

static bool is_html_link_attr(const char* html_data, size_t name_start, size_t name_end) {
    return span_case_equals(html_data, name_start, name_end, "href") || span_case_equals(html_data, name_start, name_end, "src");
}

static BxFetchHtmlLinkKind html_link_kind_for_attr(const char* html_data, size_t tag_name_start, size_t tag_name_end, size_t attr_name_start, size_t attr_name_end) {
    if (span_case_equals(html_data, attr_name_start, attr_name_end, "href") && span_case_equals(html_data, tag_name_start, tag_name_end, "a")) {
        return BX_FETCH_HTML_LINK_NAVIGATION;
    }

    return BX_FETCH_HTML_LINK_REQUISITE;
}

static bool is_html_rawtext_tag(const char* html_data, size_t name_start, size_t name_end) {
    return span_case_equals(html_data, name_start, name_end, "script") || span_case_equals(html_data, name_start, name_end, "style");
}

static size_t skip_html_comment(const char* html_data, size_t len, size_t i) {
    if ((i + 2) >= len || html_data[i] != '!' || html_data[i + 1] != '-' || html_data[i + 2] != '-') {
        return i;
    }

    i += 3;
    while ((i + 2) < len) {
        if (html_data[i] == '-' && html_data[i + 1] == '-' && html_data[i + 2] == '>') {
            return i + 3;
        }
        i++;
    }
    return len;
}

static size_t skip_html_tag_to_gt(const char* html_data, size_t len, size_t i) {
    while (i < len) {
        if (html_data[i] == '>') {
            return i + 1;
        }
        i++;
    }
    return len;
}

static size_t skip_html_rawtext_element_body(const char* html_data, size_t len, size_t i, size_t tag_name_start, size_t tag_name_end) {
    while (i < len) {
        if (html_data[i] != '<') {
            i++;
            continue;
        }

        size_t j = i + 1;
        if (j >= len || html_data[j] != '/') {
            i++;
            continue;
        }
        j++;

        while (j < len && isspace((unsigned char)html_data[j])) {
            j++;
        }

        size_t close_name_start = j;
        while (j < len && is_html_name_char((unsigned char)html_data[j])) {
            j++;
        }
        size_t close_name_end = j;
        if (!span_case_equals_span(html_data, tag_name_start, tag_name_end, close_name_start, close_name_end)) {
            i++;
            continue;
        }

        while (j < len && isspace((unsigned char)html_data[j])) {
            j++;
        }
        if (j < len && html_data[j] == '>') {
            return j + 1;
        }
        i++;
    }
    return len;
}

static int scan_html_link_attrs(const char* html_data, size_t len, HtmlAttrVisitorFn visitor, void* userdata) {
    if (!html_data || !visitor)
        return -1;

    size_t i = 0;
    while (i < len) {
        if (html_data[i] != '<') {
            i++;
            continue;
        }
        i++;
        if (i >= len)
            break;

        if (html_data[i] == '!') {
            size_t next = skip_html_comment(html_data, len, i);
            if (next != i) {
                i = next;
                continue;
            }
            i = skip_html_tag_to_gt(html_data, len, i + 1);
            continue;
        }
        if (html_data[i] == '?') {
            i = skip_html_tag_to_gt(html_data, len, i + 1);
            continue;
        }

        bool closing_tag = false;
        if (html_data[i] == '/') {
            closing_tag = true;
            i++;
        }

        while (i < len && isspace((unsigned char)html_data[i])) {
            i++;
        }
        size_t tag_name_start = i;
        while (i < len && is_html_name_char((unsigned char)html_data[i])) {
            i++;
        }
        size_t tag_name_end = i;
        if (tag_name_start == tag_name_end) {
            continue;
        }

        bool rawtext_tag = !closing_tag && is_html_rawtext_tag(html_data, tag_name_start, tag_name_end);
        if (closing_tag) {
            i = skip_html_tag_to_gt(html_data, len, i);
            continue;
        }

        bool malformed_tag = false;

        while (i < len) {
            while (i < len && isspace((unsigned char)html_data[i])) {
                i++;
            }
            if (i >= len)
                break;
            if (html_data[i] == '>') {
                i++;
                break;
            }
            if (html_data[i] == '/' && (i + 1) < len && html_data[i + 1] == '>') {
                i += 2;
                break;
            }

            size_t name_start = i;
            while (i < len && is_html_name_char((unsigned char)html_data[i])) {
                i++;
            }
            size_t name_end = i;
            if (name_start == name_end) {
                if (i < len && html_data[i] == '<') {
                    malformed_tag = true;
                    break;
                }
                i++;
                continue;
            }

            while (i < len && isspace((unsigned char)html_data[i])) {
                i++;
            }
            if (i >= len || html_data[i] != '=') {
                continue;
            }
            i++;
            while (i < len && isspace((unsigned char)html_data[i])) {
                i++;
            }
            if (i >= len)
                break;

            size_t value_start = i;
            size_t value_end = i;
            bool emit_value = true;
            bool malformed_value = false;
            if (html_data[i] == '\'' || html_data[i] == '"') {
                char quote = html_data[i++];
                value_start = i;
                while (i < len && html_data[i] != quote) {
                    if (html_data[i] == '<' || html_data[i] == '>') {
                        malformed_value = true;
                        break;
                    }
                    i++;
                }
                value_end = i;
                if (i < len && html_data[i] == quote && !malformed_value) {
                    i++;
                }
                else {
                    emit_value = false;
                    malformed_value = true;
                }
            }
            else {
                value_start = i;
                while (i < len && !isspace((unsigned char)html_data[i]) && html_data[i] != '>' && html_data[i] != '<' && html_data[i] != '\'' && html_data[i] != '"') {
                    i++;
                }
                value_end = i;
                if (value_start == value_end) {
                    emit_value = false;
                }
                if (i < len && html_data[i] == '<') {
                    malformed_value = true;
                }
            }

            if (emit_value && is_html_link_attr(html_data, name_start, name_end)) {
                if (visitor(userdata, html_data, tag_name_start, tag_name_end, name_start, name_end, value_start, value_end) != 0) {
                    return -1;
                }
            }

            if (malformed_value) {
                malformed_tag = true;
                break;
            }
        }

        if (malformed_tag) {
            i = skip_html_tag_to_gt(html_data, len, i);
        }
        if (rawtext_tag && i < len) {
            i = skip_html_rawtext_element_body(html_data, len, i, tag_name_start, tag_name_end);
        }
    }

    return 0;
}

static void extract_html_link_compat(void* userdata, const char* url, BxFetchHtmlLinkKind kind) {
    (void)kind;

    const HtmlExtractCompatContext* ctx = userdata;
    if (!ctx || !ctx->cb)
        return;
    ctx->cb(ctx->userdata, url);
}

static int html_extract_visit(void* userdata, const char* html_data, size_t tag_name_start, size_t tag_name_end, size_t attr_name_start, size_t attr_name_end, size_t value_start, size_t value_end) {
    HtmlExtractContext* ctx = userdata;
    if (!ctx || value_end < value_start)
        return -1;

    char* url = strndup(html_data + value_start, value_end - value_start);
    if (!url)
        return -1;
    ctx->cb(ctx->userdata, url, html_link_kind_for_attr(html_data, tag_name_start, tag_name_end, attr_name_start, attr_name_end));
    free(url);
    return 0;
}

int bx_fetch_html_extract_links_typed(const char* base_url, const char* html_data, size_t len, BxFetchHtmlLinkCallback cb, void* userdata) {
    (void)base_url;
    if (!cb)
        errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return -1;

    HtmlExtractContext ctx = {cb, userdata};
    return scan_html_link_attrs(html_data, len, html_extract_visit, &ctx);
}

int bx_fetch_html_extract_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkCallback cb, void* userdata) {
    HtmlExtractCompatContext ctx = {
        .cb = cb,
        .userdata = userdata,
    };
    return bx_fetch_html_extract_links_typed(base_url, html_data, len, extract_html_link_compat, &ctx);
}

static int html_rewrite_add(HtmlRewriteContext* ctx, size_t start, size_t end, char* replacement) {
    if (!ctx || !replacement || end < start)
        return -1;
    if (ctx->count == ctx->capacity) {
        size_t new_capacity = (ctx->capacity == 0) ? 8 : ctx->capacity * 2;
        HtmlReplacement* new_items = realloc(ctx->items, new_capacity * sizeof(HtmlReplacement));
        if (!new_items)
            return -1;
        ctx->items = new_items;
        ctx->capacity = new_capacity;
    }

    ctx->items[ctx->count].start = start;
    ctx->items[ctx->count].end = end;
    ctx->items[ctx->count].replacement = replacement;
    ctx->count++;
    return 0;
}

static void html_rewrite_clear(HtmlRewriteContext* ctx) {
    if (!ctx)
        return;
    for (size_t i = 0; i < ctx->count; i++) {
        free(ctx->items[i].replacement);
    }
    free(ctx->items);
    ctx->items = NULL;
    ctx->count = 0;
    ctx->capacity = 0;
}

static int html_rewrite_visit(void* userdata, const char* html_data, size_t tag_name_start, size_t tag_name_end, size_t attr_name_start, size_t attr_name_end, size_t value_start, size_t value_end) {
    (void)tag_name_start;
    (void)tag_name_end;
    (void)attr_name_start;
    (void)attr_name_end;

    HtmlRewriteContext* ctx = userdata;
    if (!ctx || value_end < value_start)
        return -1;

    char* original = strndup(html_data + value_start, value_end - value_start);
    if (!original)
        return -1;

    char* rewritten = ctx->cb(ctx->userdata, original);
    free(original);
    if (!rewritten)
        return 0;

    if (html_rewrite_add(ctx, value_start, value_end, rewritten) != 0) {
        free(rewritten);
        return -1;
    }

    return 0;
}

static int append_buffer(char** buffer, size_t* length, size_t* capacity, const char* data, size_t data_len) {
    if (!buffer || !length || !capacity || (!data && data_len > 0))
        return -1;
    if (data_len == 0)
        return 0;

    size_t needed = *length + data_len + 1;
    if (*capacity < needed) {
        size_t new_capacity = (*capacity == 0) ? needed : *capacity;
        while (new_capacity < needed) {
            new_capacity *= 2;
        }

        char* resized = realloc(*buffer, new_capacity);
        if (!resized)
            return -1;
        *buffer = resized;
        *capacity = new_capacity;
    }

    memcpy(*buffer + *length, data, data_len);
    *length += data_len;
    (*buffer)[*length] = '\0';
    return 0;
}

char* bx_fetch_html_convert_links(const char* base_url, const char* html_data, size_t len, BxFetchLinkRewriteCallback cb, void* userdata) {
    (void)base_url;
    if (!cb)
        errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return NULL;

    HtmlRewriteContext rewrite_ctx = {cb, userdata, NULL, 0, 0};
    if (scan_html_link_attrs(html_data, len, html_rewrite_visit, &rewrite_ctx) != 0) {
        html_rewrite_clear(&rewrite_ctx);
        return NULL;
    }

    if (rewrite_ctx.count == 0) {
        html_rewrite_clear(&rewrite_ctx);
        return strndup(html_data, len);
    }

    char* out = NULL;
    size_t out_len = 0;
    size_t out_cap = 0;
    size_t cursor = 0;
    bool ok = true;

    for (size_t i = 0; i < rewrite_ctx.count; i++) {
        HtmlReplacement* rep = &rewrite_ctx.items[i];
        if (rep->start < cursor || rep->end < rep->start || rep->end > len) {
            ok = false;
            break;
        }

        if (append_buffer(&out, &out_len, &out_cap, html_data + cursor, rep->start - cursor) != 0) {
            ok = false;
            break;
        }
        if (append_buffer(&out, &out_len, &out_cap, rep->replacement, strlen(rep->replacement)) != 0) {
            ok = false;
            break;
        }
        cursor = rep->end;
    }

    if (ok && append_buffer(&out, &out_len, &out_cap, html_data + cursor, len - cursor) != 0) {
        ok = false;
    }

    html_rewrite_clear(&rewrite_ctx);
    if (!ok) {
        free(out);
        return NULL;
    }

    if (!out) {
        out = strdup("");
    }
    return out;
}
#endif
