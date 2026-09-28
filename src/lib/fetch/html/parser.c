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

int bx_fetch_css_extract_links(const char* css_data, size_t len, BxFetchLinkCallback cb, void* userdata) {
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

typedef struct {
    BxFetchHtmlBaseCallback callback;
    void* userdata;
    bool seen;
} HtmlBaseContext;

#include <liblexa/html/document.h>
#include <liblexa/serialize.h>

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} NativeOutput;

static bool native_name_equals(lxa_span_t name, const char* expected) {
    size_t length = strlen(expected);
    return name.length == length && memcmp(name.data, expected, length) == 0;
}

static int native_status_error(lxa_status_t status) {
    errno = status == LXA_ERROR_NO_MEMORY ? ENOMEM :
            status == LXA_ERROR_LIMIT || status == LXA_ERROR_OVERFLOW ? EFBIG :
            status == LXA_ERROR_ENTROPY ? EIO : EINVAL;
    return -1;
}

static int native_parse(const char* bytes, size_t len, lxa_html_document_t** out) {
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_limits_t limits = lxa_limits_default();
    lxa_html_result_t result;
    lxa_limit_reason_t reason;
    limits.max_input_bytes = BX_FETCH_DOCUMENT_PARSE_MAX_BYTES;
    lxa_span_t input = {(const uint8_t*)bytes, len};
    lxa_status_t status = lxa_html_document_parse(&allocator, &limits, input, &result, &reason, out);
    (void)result;
    (void)reason;
    return status == LXA_OK ? 0 : native_status_error(status);
}

/* URL copies are callback-scoped. In particular, an attribute update can
 * relocate the document's string storage before the next lookup. */
typedef int (*NativeVisitor)(lxa_dom_nodes_t*, lxa_dom_ref_t, lxa_dom_ref_t,
                             bool, const char*, void*);

static int native_visit_attributes(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                   bool base, NativeVisitor visit, void* context) {
    static const char* const names[] = {"href", "src"};
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        lxa_span_t key = {(const uint8_t*)names[i], strlen(names[i])};
        lxa_dom_ref_t attribute;
        lxa_span_t stored, value;
        lxa_status_t status = lxa_dom_nodes_find_attribute(nodes, element, key, &attribute);
        if (status != LXA_OK)
            return native_status_error(status);
        if (!attribute.handle)
            continue;
        status = lxa_dom_nodes_attribute_read(nodes, attribute, &stored, &value);
        if (status != LXA_OK)
            return native_status_error(status);
        char* url = strndup(value.data ? (const char*)value.data : "", value.length);
        if (!url) {
            errno = ENOMEM;
            return -1;
        }
        int result = visit(nodes, element, attribute, base, url, context);
        free(url);
        if (result)
            return -1;
    }
    return 0;
}

static int native_walk(lxa_html_document_t* document, NativeVisitor visit,
                       void* context, bool base_only) {
    lxa_dom_nodes_t* nodes = lxa_html_document_nodes(document);
    lxa_dom_ref_t root = lxa_html_document_root(document), node = root;
    /* The canonical tree has no cycles; still bound the walk if malformed
     * internal links ever become observable. */
    size_t steps = 0, count = lxa_dom_nodes_length(nodes);
    for (;;) {
        lxa_dom_record_t record;
        if (++steps > count * 3u + 1u)
            return native_status_error(LXA_ERROR_ARGUMENT);
        lxa_status_t status = lxa_dom_nodes_read(nodes, node, &record);
        if (status != LXA_OK)
            return native_status_error(status);
        if (record.kind == LXA_DOM_KIND_ELEMENT) {
            lxa_span_t tag;
            status = lxa_dom_nodes_element_name(nodes, node, &tag);
            if (status != LXA_OK)
                return native_status_error(status);
            bool base = native_name_equals(tag, "base");
            if ((!base_only || base) && native_visit_attributes(nodes, node, base, visit, context))
                return -1;
        }
        if (record.kind == LXA_DOM_KIND_DOCUMENT || record.kind == LXA_DOM_KIND_ELEMENT) {
            lxa_dom_ref_t child;
            status = lxa_dom_nodes_first_child(nodes, node, &child);
            if (status != LXA_OK)
                return native_status_error(status);
            if (child.handle) {
                node = child;
                continue;
            }
        }
        for (;;) {
            if (node.handle == root.handle)
                return 0;
            lxa_dom_ref_t next;
            status = lxa_dom_nodes_next_sibling(nodes, node, &next);
            if (status != LXA_OK)
                return native_status_error(status);
            if (next.handle) {
                node = next;
                break;
            }
            status = lxa_dom_nodes_parent(nodes, node, &node);
            if (status != LXA_OK || !node.handle)
                return native_status_error(status);
        }
    }
}

static int native_base_visit(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                             lxa_dom_ref_t attribute, bool base, const char* url, void* context) {
    (void)element;
    HtmlBaseContext* state = context;
    if (!base || state->seen)
        return 0;
    lxa_span_t key, value;
    lxa_status_t status = lxa_dom_nodes_attribute_read(nodes, attribute, &key, &value);
    if (status != LXA_OK)
        return native_status_error(status);
    if (!native_name_equals(key, "href"))
        return 0;
    state->seen = true;
    if (state->callback(state->userdata, url) == 0)
        return 0;
    errno = ECANCELED;
    return -1;
}

typedef struct {
    BxFetchHtmlLinkCallback cb;
    void* userdata;
} NativeExtract;

static int native_extract_visit(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                lxa_dom_ref_t attribute, bool base, const char* url, void* context) {
    if (base)
        return 0;
    NativeExtract* state = context;
    lxa_span_t name, key, value;
    lxa_status_t status = lxa_dom_nodes_element_name(nodes, element, &name);
    if (status != LXA_OK)
        return native_status_error(status);
    bool anchor = native_name_equals(name, "a");
    status = lxa_dom_nodes_attribute_read(nodes, attribute, &key, &value);
    if (status != LXA_OK)
        return native_status_error(status);
    state->cb(state->userdata, url,
              anchor && native_name_equals(key, "href")
                  ? BX_FETCH_HTML_LINK_NAVIGATION : BX_FETCH_HTML_LINK_REQUISITE);
    return 0;
}

typedef struct {
    BxFetchLinkRewriteCallback cb;
    void* userdata;
    bool reset_base;
} NativeRewrite;

static int native_rewrite_visit(lxa_dom_nodes_t* nodes, lxa_dom_ref_t element,
                                lxa_dom_ref_t attribute, bool base, const char* url, void* context) {
    (void)element;
    NativeRewrite* state = context;
    lxa_span_t key, value;
    lxa_status_t status = lxa_dom_nodes_attribute_read(nodes, attribute, &key, &value);
    if (status != LXA_OK)
        return native_status_error(status);
    bool href = native_name_equals(key, "href");
    if (base && (!state->reset_base || !href))
        return 0;
    char* replacement = base ? strdup("./") : state->cb(state->userdata, url);
    if (!replacement) {
        if (base) {
            errno = ENOMEM;
            return -1;
        }
        return 0;
    }
    lxa_span_t updated = {(const uint8_t*)replacement, strlen(replacement)};
    status = lxa_dom_nodes_attribute_set_value(nodes, attribute, updated);
    free(replacement);
    return status == LXA_OK ? 0 : native_status_error(status);
}

static lxa_write_result_t native_serialize_write(void* context, lxa_span_t bytes) {
    NativeOutput* out = context;
    lxa_write_result_t result = {0};
    if (bytes.length > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES - out->length) {
        errno = EFBIG;
        result.status = LXA_ERROR_LIMIT;
        return result;
    }
    size_t required = out->length + bytes.length + 1u;
    if (required > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 4096u;
        while (capacity < required)
            capacity = capacity > (BX_FETCH_DOCUMENT_PARSE_MAX_BYTES + 1u) / 2u
                ? BX_FETCH_DOCUMENT_PARSE_MAX_BYTES + 1u : capacity * 2u;
        char* grown = realloc(out->data, capacity);
        if (!grown) {
            errno = ENOMEM;
            result.status = LXA_ERROR_NO_MEMORY;
            return result;
        }
        out->data = grown;
        out->capacity = capacity;
    }
    if (bytes.length)
        memcpy(out->data + out->length, bytes.data, bytes.length);
    out->length += bytes.length;
    out->data[out->length] = '\0';
    result.consumed = bytes.length;
    result.status = LXA_OK;
    return result;
}

int bx_fetch_html_extract_links(const char* html_data, size_t len,
                                BxFetchHtmlLinkCallback cb, void* userdata,
                                BxFetchHtmlBaseCallback base_cb) {
    if (!cb) errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return -1;
    lxa_html_document_t* document = NULL;
    if (native_parse(html_data, len, &document))
        return -1;
    HtmlBaseContext base = {.callback = base_cb, .userdata = userdata};
    NativeExtract state = {.cb = cb, .userdata = userdata};
    int result = base_cb && native_walk(document, native_base_visit, &base, true)
        ? -1 : native_walk(document, native_extract_visit, &state, false);
    lxa_html_document_destroy(document);
    return result;
}

char* bx_fetch_html_convert_links(const char* html_data, size_t len,
                                  BxFetchLinkRewriteCallback cb, void* userdata,
                                  BxFetchHtmlBaseCallback base_cb) {
    if (!cb) errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return NULL;
    lxa_html_document_t* document = NULL;
    if (native_parse(html_data, len, &document))
        return NULL;
    HtmlBaseContext base = {.callback = base_cb, .userdata = userdata};
    NativeRewrite state = {.cb = cb, .userdata = userdata, .reset_base = base_cb != NULL};
    int result = base_cb && native_walk(document, native_base_visit, &base, true)
        ? -1 : native_walk(document, native_rewrite_visit, &state, false);
    NativeOutput output = {0};
    if (!result) {
        lxa_limit_reason_t reason;
        errno = 0;
        lxa_status_t status = lxa_html_serialize_document(
            lxa_html_document_nodes(document), lxa_html_document_root(document),
            native_serialize_write, &output, &reason);
        if (status != LXA_OK) {
            if (!errno) native_status_error(status);
            result = -1;
        }
    }
    lxa_html_document_destroy(document);
    if (result) {
        free(output.data);
        return NULL;
    }
    if (!output.data)
        output.data = strdup("");
    return output.data;
}
