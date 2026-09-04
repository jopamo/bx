#define _GNU_SOURCE
#include "lib/fetch/response.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

    free(resp->effective_url);
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
