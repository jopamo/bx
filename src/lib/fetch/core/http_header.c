#define _GNU_SOURCE
#include "lib/fetch/http_header.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char* name;
    size_t name_len;
    const char* value;
    size_t value_len;
} BxFetchHttpHeaderView;

static bool ascii_is_tchar(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '-' ||
           c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}

bool bx_fetch_http_method_is_valid(const char* method) {
    if (!method || method[0] == '\0')
        return false;
    for (const unsigned char* p = (const unsigned char*)method; *p; p++) {
        if (!ascii_is_tchar(*p))
            return false;
    }
    return true;
}

static unsigned char ascii_lower(unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

static bool span_equals_case(const char* value, size_t value_len, const char* expected) {
    if (!value || !expected || strlen(expected) != value_len)
        return false;
    for (size_t i = 0; i < value_len; i++) {
        if (ascii_lower((unsigned char)value[i]) != ascii_lower((unsigned char)expected[i])) {
            return false;
        }
    }
    return true;
}

static void skip_ows(const char** cursor) {
    while (**cursor == ' ' || **cursor == '\t')
        (*cursor)++;
}

static bool parse_token_span(const char** cursor, const char** start_out, size_t* length_out) {
    const char* start = *cursor;
    while (ascii_is_tchar((unsigned char)**cursor))
        (*cursor)++;
    if (*cursor == start)
        return false;
    *start_out = start;
    *length_out = (size_t)(*cursor - start);
    return true;
}

typedef struct {
    const char* start;
    size_t length;
    bool quoted;
} HeaderParameterValue;

static bool parse_parameter_value(const char** cursor, HeaderParameterValue* value_out) {
    if (**cursor != '"') {
        value_out->quoted = false;
        return parse_token_span(cursor, &value_out->start, &value_out->length);
    }

    (*cursor)++;
    const char* start = *cursor;
    while (**cursor != '\0' && **cursor != '"') {
        unsigned char c = (unsigned char)**cursor;
        if (c < 0x20 || c == 0x7f)
            return false;
        if (c == '\\') {
            (*cursor)++;
            c = (unsigned char)**cursor;
            if (c == '\0' || c < 0x20 || c == 0x7f)
                return false;
        }
        (*cursor)++;
    }
    if (**cursor != '"')
        return false;

    value_out->start = start;
    value_out->length = (size_t)(*cursor - start);
    value_out->quoted = true;
    (*cursor)++;
    return true;
}

static bool span_has_safe_filename_bytes(const char* value, size_t length) {
    if (!value || length == 0)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

static char* decode_parameter_value(const HeaderParameterValue* value) {
    if (!value) {
        errno = EINVAL;
        return NULL;
    }

    char* decoded = malloc(value->length + 1u);
    if (!decoded)
        return NULL;

    size_t out = 0;
    for (size_t i = 0; i < value->length; i++) {
        if (value->quoted && value->start[i] == '\\') {
            i++;
        }
        decoded[out++] = value->start[i];
    }
    decoded[out] = '\0';

    if (!span_has_safe_filename_bytes(decoded, out)) {
        free(decoded);
        errno = EINVAL;
        return NULL;
    }
    return decoded;
}

static int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    c = ascii_lower(c);
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    return -1;
}

static bool ascii_is_attr_char(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '!' || c == '#' || c == '$' || c == '&' || c == '+' || c == '-' || c == '.' || c == '^' || c == '_' ||
           c == '`' || c == '|' || c == '~';
}

static bool span_is_utf8(const unsigned char* value, size_t length) {
    size_t i = 0;
    while (i < length) {
        unsigned char c = value[i++];
        if (c <= 0x7f)
            continue;

        size_t continuation_count = 0;
        unsigned char second_min = 0x80;
        unsigned char second_max = 0xbf;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation_count = 1;
        }
        else if (c >= 0xe0 && c <= 0xef) {
            continuation_count = 2;
            if (c == 0xe0)
                second_min = 0xa0;
            if (c == 0xed)
                second_max = 0x9f;
        }
        else if (c >= 0xf0 && c <= 0xf4) {
            continuation_count = 3;
            if (c == 0xf0)
                second_min = 0x90;
            if (c == 0xf4)
                second_max = 0x8f;
        }
        else {
            return false;
        }

        if (continuation_count > length - i || value[i] < second_min || value[i] > second_max) {
            return false;
        }
        i++;
        for (size_t j = 1; j < continuation_count; j++, i++) {
            if (value[i] < 0x80 || value[i] > 0xbf)
                return false;
        }
    }
    return true;
}

static char* decode_extended_filename(const HeaderParameterValue* value) {
    if (!value || value->quoted) {
        errno = EINVAL;
        return NULL;
    }

    const char* end = value->start + value->length;
    const char* first_quote = memchr(value->start, '\'', value->length);
    if (!first_quote) {
        errno = EINVAL;
        return NULL;
    }
    const char* second_quote = memchr(first_quote + 1, '\'', (size_t)(end - (first_quote + 1)));
    if (!second_quote || !span_equals_case(value->start, (size_t)(first_quote - value->start), "UTF-8")) {
        errno = EINVAL;
        return NULL;
    }

    for (const char* p = first_quote + 1; p < second_quote; p++) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-')) {
            errno = EINVAL;
            return NULL;
        }
    }

    const char* encoded = second_quote + 1;
    char* decoded = malloc((size_t)(end - encoded) + 1u);
    if (!decoded)
        return NULL;

    size_t out = 0;
    for (const char* p = encoded; p < end; p++) {
        unsigned char byte = (unsigned char)*p;
        if (byte == '%') {
            if (end - p < 3) {
                free(decoded);
                errno = EINVAL;
                return NULL;
            }
            int hi = hex_nibble((unsigned char)p[1]);
            int lo = hex_nibble((unsigned char)p[2]);
            if (hi < 0 || lo < 0) {
                free(decoded);
                errno = EINVAL;
                return NULL;
            }
            byte = (unsigned char)((hi << 4) | lo);
            p += 2;
        }
        else if (!ascii_is_attr_char(byte)) {
            free(decoded);
            errno = EINVAL;
            return NULL;
        }
        decoded[out++] = (char)byte;
    }
    decoded[out] = '\0';

    if (!span_has_safe_filename_bytes(decoded, out) || !span_is_utf8((const unsigned char*)decoded, out)) {
        free(decoded);
        errno = EINVAL;
        return NULL;
    }
    return decoded;
}

static bool framing_name_is_forbidden(const char* name, size_t name_len) {
    return span_equals_case(name, name_len, "Content-Length") || span_equals_case(name, name_len, "Transfer-Encoding") || span_equals_case(name, name_len, "Trailer");
}

static BxFetchHttpHeaderError validate_name(const char* name, size_t name_len) {
    if (!name || name_len == 0)
        return BX_FETCH_HTTP_HEADER_INVALID_NAME;
    for (size_t i = 0; i < name_len; i++) {
        if (!ascii_is_tchar((unsigned char)name[i])) {
            return BX_FETCH_HTTP_HEADER_INVALID_NAME;
        }
    }
    if (framing_name_is_forbidden(name, name_len)) {
        return BX_FETCH_HTTP_HEADER_FORBIDDEN_FRAMING;
    }
    return BX_FETCH_HTTP_HEADER_OK;
}

static BxFetchHttpHeaderError normalize_value_span(const char* value, size_t value_len, const char** trimmed_out, size_t* trimmed_len_out) {
    if (!value || !trimmed_out || !trimmed_len_out) {
        return BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < value_len; i++) {
        unsigned char c = (unsigned char)value[i];
        if ((c < 0x20 && c != '\t') || c == 0x7f) {
            return BX_FETCH_HTTP_HEADER_INVALID_VALUE;
        }
    }

    const char* start = value;
    const char* end = value + value_len;
    while (start < end && (*start == ' ' || *start == '\t'))
        start++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    *trimmed_out = start;
    *trimmed_len_out = (size_t)(end - start);
    return BX_FETCH_HTTP_HEADER_OK;
}

static BxFetchHttpHeaderError parse_line(const char* line, BxFetchHttpHeaderView* view) {
    if (!line || !view)
        return BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT;

    const char* colon = strchr(line, ':');
    if (!colon)
        return BX_FETCH_HTTP_HEADER_MALFORMED_LINE;

    size_t name_len = (size_t)(colon - line);
    BxFetchHttpHeaderError error = validate_name(line, name_len);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;

    const char* value = colon + 1;
    const char* trimmed = NULL;
    size_t trimmed_len = 0;
    error = normalize_value_span(value, strlen(value), &trimmed, &trimmed_len);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;

    *view = (BxFetchHttpHeaderView){
        .name = line,
        .name_len = name_len,
        .value = trimmed,
        .value_len = trimmed_len,
    };
    return BX_FETCH_HTTP_HEADER_OK;
}

static BxFetchHttpHeaderError view_pair(const char* name, const char* value, BxFetchHttpHeaderView* view) {
    if (!name || !value || !view) {
        return BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT;
    }

    size_t name_len = strlen(name);
    BxFetchHttpHeaderError error = validate_name(name, name_len);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;

    const char* trimmed = NULL;
    size_t trimmed_len = 0;
    error = normalize_value_span(value, strlen(value), &trimmed, &trimmed_len);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;

    *view = (BxFetchHttpHeaderView){
        .name = name,
        .name_len = name_len,
        .value = trimmed,
        .value_len = trimmed_len,
    };
    return BX_FETCH_HTTP_HEADER_OK;
}

static BxFetchHttpHeaderError format_view(const BxFetchHttpHeaderView* view, bool curl_format, char** formatted_out) {
    if (!view || !formatted_out) {
        return BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT;
    }
    *formatted_out = NULL;

    size_t separator_len = view->value_len > 0 ? 2 : 1;
    if (view->name_len > SIZE_MAX - separator_len || view->name_len + separator_len > SIZE_MAX - view->value_len) {
        return BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY;
    }
    size_t formatted_len = view->name_len + separator_len + view->value_len;
    if (formatted_len == SIZE_MAX)
        return BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY;

    char* formatted = calloc(formatted_len + 1, 1);
    if (!formatted)
        return BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY;

    memcpy(formatted, view->name, view->name_len);
    if (view->value_len == 0) {
        formatted[view->name_len] = curl_format ? ';' : ':';
    }
    else {
        formatted[view->name_len] = ':';
        formatted[view->name_len + 1] = ' ';
        memcpy(formatted + view->name_len + 2, view->value, view->value_len);
    }

    *formatted_out = formatted;
    return BX_FETCH_HTTP_HEADER_OK;
}

const char* bx_fetch_http_header_error_string(BxFetchHttpHeaderError error) {
    switch (error) {
        case BX_FETCH_HTTP_HEADER_OK:
            return NULL;
        case BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT:
            return "missing HTTP header input";
        case BX_FETCH_HTTP_HEADER_MALFORMED_LINE:
            return "HTTP header must contain a field name followed by ':'";
        case BX_FETCH_HTTP_HEADER_INVALID_NAME:
            return "HTTP field name must use RFC token characters";
        case BX_FETCH_HTTP_HEADER_INVALID_VALUE:
            return "HTTP field value contains a forbidden control character";
        case BX_FETCH_HTTP_HEADER_FORBIDDEN_FRAMING:
            return "Content-Length, Transfer-Encoding, and Trailer are managed by Mira";
        case BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY:
            return "out of memory while preparing HTTP headers";
    }
    return "invalid HTTP header";
}

BxFetchHttpHeaderError bx_fetch_http_header_normalize_line(const char* line, char** normalized_out) {
    if (normalized_out)
        *normalized_out = NULL;
    BxFetchHttpHeaderView view;
    BxFetchHttpHeaderError error = parse_line(line, &view);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;
    return format_view(&view, false, normalized_out);
}

BxFetchHttpHeaderError bx_fetch_http_header_normalize_pair(const char* name, const char* value, char** normalized_name_out, char** normalized_value_out) {
    if (normalized_name_out)
        *normalized_name_out = NULL;
    if (normalized_value_out)
        *normalized_value_out = NULL;
    if (!normalized_name_out || !normalized_value_out) {
        return BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT;
    }

    BxFetchHttpHeaderView view;
    BxFetchHttpHeaderError error = view_pair(name, value, &view);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;

    char* normalized_name = strndup(view.name, view.name_len);
    if (!normalized_name)
        return BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY;
    char* normalized_value = strndup(view.value, view.value_len);
    if (!normalized_value) {
        free(normalized_name);
        return BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY;
    }

    *normalized_name_out = normalized_name;
    *normalized_value_out = normalized_value;
    return BX_FETCH_HTTP_HEADER_OK;
}

BxFetchHttpHeaderError bx_fetch_http_header_format_line_for_curl(const char* line, char** formatted_out) {
    if (formatted_out)
        *formatted_out = NULL;
    BxFetchHttpHeaderView view;
    BxFetchHttpHeaderError error = parse_line(line, &view);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;
    return format_view(&view, true, formatted_out);
}

BxFetchHttpHeaderError bx_fetch_http_header_format_pair_for_curl(const char* name, const char* value, char** formatted_out) {
    if (formatted_out)
        *formatted_out = NULL;
    BxFetchHttpHeaderView view;
    BxFetchHttpHeaderError error = view_pair(name, value, &view);
    if (error != BX_FETCH_HTTP_HEADER_OK)
        return error;
    return format_view(&view, true, formatted_out);
}

BxFetchContentDispositionResult bx_fetch_http_content_disposition_filename(const char* value, char** filename_out) {
    if (filename_out)
        *filename_out = NULL;
    if (!value || !filename_out) {
        return BX_FETCH_CONTENT_DISPOSITION_INVALID;
    }

    const char* cursor = value;
    skip_ows(&cursor);
    const char* disposition = NULL;
    size_t disposition_length = 0;
    if (!parse_token_span(&cursor, &disposition, &disposition_length)) {
        return BX_FETCH_CONTENT_DISPOSITION_INVALID;
    }
    (void)disposition;
    (void)disposition_length;
    skip_ows(&cursor);

    char* filename = NULL;
    char* extended_filename = NULL;
    bool saw_filename = false;
    bool saw_extended_filename = false;
    while (*cursor != '\0') {
        if (*cursor != ';')
            goto invalid;
        cursor++;
        skip_ows(&cursor);
        if (*cursor == '\0')
            goto invalid;

        const char* name = NULL;
        size_t name_length = 0;
        if (!parse_token_span(&cursor, &name, &name_length))
            goto invalid;
        skip_ows(&cursor);
        if (*cursor != '=')
            goto invalid;
        cursor++;
        skip_ows(&cursor);

        HeaderParameterValue parameter_value = {0};
        if (!parse_parameter_value(&cursor, &parameter_value))
            goto invalid;
        skip_ows(&cursor);
        if (*cursor != '\0' && *cursor != ';')
            goto invalid;

        bool is_filename = span_equals_case(name, name_length, "filename");
        bool is_extended_filename = span_equals_case(name, name_length, "filename*");
        if (!is_filename && !is_extended_filename)
            continue;

        if ((is_filename && saw_filename) || (is_extended_filename && saw_extended_filename)) {
            goto invalid;
        }

        errno = 0;
        char* decoded = is_extended_filename ? decode_extended_filename(&parameter_value) : decode_parameter_value(&parameter_value);
        if (!decoded) {
            if (errno == ENOMEM)
                goto out_of_memory;
            goto invalid;
        }

        if (is_extended_filename) {
            saw_extended_filename = true;
            extended_filename = decoded;
        }
        else {
            saw_filename = true;
            filename = decoded;
        }
    }

    if (extended_filename) {
        free(filename);
        *filename_out = extended_filename;
        return BX_FETCH_CONTENT_DISPOSITION_FILENAME;
    }
    if (filename) {
        *filename_out = filename;
        return BX_FETCH_CONTENT_DISPOSITION_FILENAME;
    }
    return BX_FETCH_CONTENT_DISPOSITION_NONE;

out_of_memory:
    free(filename);
    free(extended_filename);
    return BX_FETCH_CONTENT_DISPOSITION_OUT_OF_MEMORY;

invalid:
    free(filename);
    free(extended_filename);
    return BX_FETCH_CONTENT_DISPOSITION_INVALID;
}
