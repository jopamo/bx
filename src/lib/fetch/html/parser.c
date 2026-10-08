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
    int error;
} HtmlBaseContext;

#include <liblexa/html_rewrite.h>

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
} SourceOutput;

static bool html_name_equals(lxa_span_t name, const char* expected) {
    size_t length = strlen(expected);
    return name.length == length && memcmp(name.data, expected, length) == 0;
}

static int html_status_error(lxa_status_t status) {
    errno = status == LXA_ERROR_NO_MEMORY ? ENOMEM :
            status == LXA_ERROR_LIMIT || status == LXA_ERROR_OVERFLOW ? EFBIG :
            status == LXA_ERROR_ENTROPY ? EIO : EINVAL;
    return -1;
}

typedef struct {
    BxFetchHtmlLinkCallback cb;
    void* userdata;
} SourceExtract;

static lxa_status_t source_base(void* context, const lxa_html_source_attribute_t* event) {
    HtmlBaseContext* state = context;
    if (state->seen)
        return LXA_OK;
    char* url = strndup(event->value.data ? (const char*)event->value.data : "",
                       event->value.length);
    if (!url)
        return LXA_ERROR_NO_MEMORY;
    state->seen = true;
    int result = state->callback(state->userdata, url);
    free(url);
    if (!result)
        return LXA_OK;
    state->error = ECANCELED;
    return LXA_ERROR_ARGUMENT;
}

static lxa_status_t source_link(void* context, const lxa_html_source_attribute_t* event) {
    if (html_name_equals(event->tag, "base"))
        return LXA_OK;
    SourceExtract* state = context;
    char* url = strndup(event->value.data ? (const char*)event->value.data : "",
                       event->value.length);
    if (!url)
        return LXA_ERROR_NO_MEMORY;
    state->cb(state->userdata, url,
              html_name_equals(event->tag, "a") && html_name_equals(event->name, "href")
                  ? BX_FETCH_HTML_LINK_NAVIGATION : BX_FETCH_HTML_LINK_REQUISITE);
    free(url);
    return LXA_OK;
}

typedef struct {
    BxFetchLinkRewriteCallback cb;
    void* userdata;
    bool reset_base;
    char* pending;
} SourceRewrite;

static const lxa_html_source_target_t link_targets[] = {
    {{NULL, 0}, {(const uint8_t*)"href", 4}},
    {{NULL, 0}, {(const uint8_t*)"src", 3}}
};

static int source_first_base(const lxa_allocator_t* allocator,
                             const lxa_limits_t* limits, lxa_span_t input,
                             HtmlBaseContext* base) {
    if (!base->callback)
        return 0;
    const lxa_html_source_target_t target = {
        {(const uint8_t*)"base", 4}, {(const uint8_t*)"href", 4}
    };
    lxa_html_result_t result;
    lxa_status_t status = lxa_html_extract_attributes(allocator, limits, input, NULL,
        &target, 1, source_base, base, &result, NULL);
    if (status == LXA_OK)
        return 0;
    if (result == LXA_HTML_CALLBACK_FAILURE && base->error) {
        errno = base->error;
        return -1;
    }
    return html_status_error(status);
}

static lxa_status_t source_rewrite(void* context,
                                   const lxa_html_source_attribute_t* event,
                                   lxa_html_source_replacement_t* replacement) {
    SourceRewrite* state = context;
    /* The preceding replacement has been consumed before this decision. */
    free(state->pending);
    state->pending = NULL;
    if (html_name_equals(event->tag, "base")) {
        if (state->reset_base && html_name_equals(event->name, "href")) {
            replacement->replace = true;
            replacement->value = (lxa_span_t){(const uint8_t*)"./", 2};
        }
        return LXA_OK;
    }
    char* url = strndup(event->value.data ? (const char*)event->value.data : "",
                       event->value.length);
    if (!url)
        return LXA_ERROR_NO_MEMORY;
    state->pending = state->cb(state->userdata, url);
    free(url);
    if (state->pending) {
        replacement->replace = true;
        replacement->value = (lxa_span_t){(const uint8_t*)state->pending,
                                         strlen(state->pending)};
    }
    return LXA_OK;
}

static lxa_write_result_t source_write(void* context, lxa_span_t bytes) {
    SourceOutput* out = context;
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
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_limits_t limits = lxa_limits_default();
    limits.max_input_bytes = BX_FETCH_DOCUMENT_PARSE_MAX_BYTES;
    limits.max_nodes = 0;
    lxa_span_t input = {(const uint8_t*)html_data, len};
    lxa_html_result_t result;
    lxa_status_t status;
    HtmlBaseContext base = {.callback = base_cb, .userdata = userdata};
    if (source_first_base(&allocator, &limits, input, &base))
        return -1;
    SourceExtract state = {.cb = cb, .userdata = userdata};
    status = lxa_html_extract_attributes(&allocator, &limits, input, NULL,
        link_targets, 2, source_link, &state, &result, NULL);
    return status == LXA_OK ? 0 : html_status_error(status);
}

char* bx_fetch_html_convert_links(const char* html_data, size_t len,
                                  BxFetchLinkRewriteCallback cb, void* userdata,
                                  BxFetchHtmlBaseCallback base_cb, size_t* output_len) {
    if (output_len)
        *output_len = 0;
    if (!cb) errno = EINVAL;
    if (!cb || !document_parser_input_valid(html_data, len))
        return NULL;
    lxa_allocator_t allocator = lxa_allocator_default();
    lxa_limits_t limits = lxa_limits_default();
    limits.max_input_bytes = BX_FETCH_DOCUMENT_PARSE_MAX_BYTES;
    limits.max_nodes = 0;
    lxa_span_t input = {(const uint8_t*)html_data, len};
    HtmlBaseContext base = {.callback = base_cb, .userdata = userdata};
    if (source_first_base(&allocator, &limits, input, &base))
        return NULL;
    SourceRewrite state = {.cb = cb, .userdata = userdata, .reset_base = base_cb != NULL};
    SourceOutput output = {0};
    lxa_html_result_t result;
    errno = 0;
    lxa_status_t status = lxa_html_rewrite_attributes(&allocator, &limits, input, NULL,
        link_targets, 2, source_rewrite, &state, source_write, &output, &result, NULL);
    free(state.pending);
    if (status != LXA_OK) {
        html_status_error(status);
        free(output.data);
        return NULL;
    }
    if (!output.data)
        output.data = strdup("");
    if (!output.data) {
        errno = ENOMEM;
        return NULL;
    }
    if (output_len)
        *output_len = output.length;
    return output.data;
}
