#define _GNU_SOURCE
#include "lib/fetch/response.h"
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

BxFetchResponse* bx_fetch_response_new(void) {
    BxFetchResponse* resp = calloc(1, sizeof(BxFetchResponse));
    if (!resp)
        return NULL;

    resp->error_number = -1;
    return resp;
}

void bx_fetch_response_free(BxFetchResponse* resp) {
    if (!resp)
        return;

    bx_fetch_prepared_url_free(resp->effective_target);
    free(resp->content_type);
    free(resp->transport_error_detail);

    if (resp->headers) {
        for (size_t i = 0; i < resp->header_count; i++) {
            free(resp->headers[i].name);
            free(resp->headers[i].value);
        }
        free(resp->headers);
    }

    free(resp);
}

const BxFetchPreparedUrl* bx_fetch_response_effective_target(const BxFetchResponse* response) {
    return response ? response->effective_target : NULL;
}

const char* bx_fetch_response_effective_url(const BxFetchResponse* response) {
    return response ? bx_fetch_prepared_url_transport(response->effective_target) : NULL;
}

int bx_fetch_response_add_header(BxFetchResponse* resp, const char* name, const char* value) {
    if (!resp || !name || !value) {
        errno = EINVAL;
        return -1;
    }

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (name_len > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES - 4 || value_len > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES - 4 - name_len) {
        errno = EFBIG;
        return -1;
    }
    size_t wire_bytes = name_len + value_len + 4;
    if (resp->header_count >= BX_FETCH_RESPONSE_HEADER_MAX_FIELDS || resp->header_bytes > BX_FETCH_RESPONSE_HEADER_BLOCK_MAX_BYTES - wire_bytes) {
        errno = EFBIG;
        return -1;
    }

    char* name_copy = strdup(name);
    char* value_copy = strdup(value);
    if (!name_copy || !value_copy) {
        free(name_copy);
        free(value_copy);
        errno = ENOMEM;
        return -1;
    }

    if (resp->header_count >= resp->header_capacity) {
        if (resp->header_capacity > SIZE_MAX / 2) {
            free(name_copy);
            free(value_copy);
            errno = ENOMEM;
            return -1;
        }
        size_t new_cap = resp->header_capacity == 0 ? 8 : resp->header_capacity * 2;
        if (new_cap > BX_FETCH_RESPONSE_HEADER_MAX_FIELDS) {
            new_cap = BX_FETCH_RESPONSE_HEADER_MAX_FIELDS;
        }
        if (new_cap <= resp->header_count || new_cap > SIZE_MAX / sizeof(BxFetchHeader)) {
            free(name_copy);
            free(value_copy);
            errno = ENOMEM;
            return -1;
        }
        BxFetchHeader* new_headers = realloc(resp->headers, new_cap * sizeof(BxFetchHeader));
        if (!new_headers) {
            free(name_copy);
            free(value_copy);
            errno = ENOMEM;
            return -1;
        }
        resp->headers = new_headers;
        resp->header_capacity = new_cap;
    }

    resp->headers[resp->header_count].name = name_copy;
    resp->headers[resp->header_count].value = value_copy;
    resp->header_count++;
    resp->header_bytes += wire_bytes;
    return 0;
}

const char* bx_fetch_response_header_value(const BxFetchResponse* response, const char* name) {
    if (!response || !name)
        return NULL;

    for (size_t i = response->header_count; i > 0; i--) {
        const BxFetchHeader* header = &response->headers[i - 1];
        if (header->name && header->value && strcasecmp(header->name, name) == 0)
            return header->value;
    }
    return NULL;
}

bool bx_fetch_content_type_equals(const char* content_type, const char* expected) {
    if (!content_type || !expected)
        return false;

    size_t content_length = strnlen(content_type, BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES + 1u);
    size_t expected_length = strnlen(expected, BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES + 1u);
    if (content_length > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES ||
        expected_length == 0 ||
        expected_length > BX_FETCH_RESPONSE_HEADER_LINE_MAX_BYTES) {
        return false;
    }

    while (content_length > 0 && isspace((unsigned char)*content_type)) {
        content_type++;
        content_length--;
    }
    const char* parameters = memchr(content_type, ';', content_length);
    size_t media_type_length = parameters ? (size_t)(parameters - content_type) : content_length;
    while (media_type_length > 0 && isspace((unsigned char)content_type[media_type_length - 1u]))
        media_type_length--;

    return media_type_length == expected_length &&
           strncasecmp(content_type, expected, media_type_length) == 0;
}

const char* bx_fetch_response_header_policy_failure_summary(BxFetchResponseHeaderPolicyFailure failure) {
    switch (failure) {
        case BX_FETCH_RESPONSE_HEADER_POLICY_LINE_TOO_LARGE:
            return "response header line exceeds " BX_FETCH_RESPONSE_HEADER_LINE_LIMIT_TEXT " limit";
        case BX_FETCH_RESPONSE_HEADER_POLICY_BLOCK_TOO_LARGE:
            return "response header block exceeds " BX_FETCH_RESPONSE_HEADER_BLOCK_LIMIT_TEXT " limit";
        case BX_FETCH_RESPONSE_HEADER_POLICY_TOO_MANY_FIELDS:
            return "response header block exceeds " BX_FETCH_RESPONSE_HEADER_FIELD_LIMIT_TEXT " field limit";
        case BX_FETCH_RESPONSE_HEADER_POLICY_OK:
        default:
            return NULL;
    }
}
