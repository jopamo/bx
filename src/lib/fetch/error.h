#ifndef BX_FETCH_ERROR_H
#define BX_FETCH_ERROR_H

/* BX_FETCH_HEADER_OWNER: util */
/* BX_FETCH_HEADER_CONSUMERS: util, entry, cli, core, runtime, policy, net, fs, store, crawl */

/*
 * Layering contract:
 * - Central error taxonomy shared across layers.
 * - Lower layers emit typed failures; entry/core map them to user-visible exits.
 *
 * Ownership and lifetime:
 * - Error enums are value types.
 * - MiraStructuredError string fields are borrowed pointers; emitters do not
 *   copy or free them.
 * - Pointers supplied to bx_fetch_error_emit_structured() must remain valid for the
 *   duration of the call.
 */

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    BX_FETCH_OK = 0,
    BX_FETCH_ERROR_INVALID_ARGUMENT,
    BX_FETCH_ERROR_MEMORY,
    BX_FETCH_ERROR_IO,
    BX_FETCH_ERROR_NETWORK,
    BX_FETCH_ERROR_HTTP,
    BX_FETCH_ERROR_SSL,
    BX_FETCH_ERROR_TIMEOUT,
    BX_FETCH_ERROR_CANCELLED,
    BX_FETCH_ERROR_UNSUPPORTED,
    BX_FETCH_ERROR_INTERNAL,
    BX_FETCH_ERROR_RESOURCE_LIMIT,
} MiraError;

const char* bx_fetch_error_string(MiraError err);

typedef enum {
    BX_FETCH_ERROR_CLASS_PARSE = 0,
    BX_FETCH_ERROR_CLASS_POLICY,
    BX_FETCH_ERROR_CLASS_HTTP,
    BX_FETCH_ERROR_CLASS_TLS,
    BX_FETCH_ERROR_CLASS_CURL_TRANSPORT,
    BX_FETCH_ERROR_CLASS_FILESYSTEM,
    BX_FETCH_ERROR_CLASS_STATE_STORE,
    BX_FETCH_ERROR_CLASS_INTERNAL,
} MiraErrorClass;

typedef enum {
    BX_FETCH_TRANSPORT_ERROR_NONE = 0,
    BX_FETCH_TRANSPORT_ERROR_NETWORK,
    BX_FETCH_TRANSPORT_ERROR_PROTOCOL,
    BX_FETCH_TRANSPORT_ERROR_TLS_RETRYABLE,
    BX_FETCH_TRANSPORT_ERROR_TLS_FATAL,
} MiraTransportErrorKind;

typedef struct {
    MiraErrorClass class_id;
    const char* summary;
    const char* url;
    const char* path;
    int http_status;
    int curl_code;
    int error_number;
    bool retryable;
    int attempt;
    int max_attempts;
} MiraStructuredError;

const char* bx_fetch_error_class_string(MiraErrorClass class_id);
MiraStructuredError bx_fetch_error_make_simple(MiraErrorClass class_id, const char* summary, const char* url, const char* path, int curl_code, int error_number);
void bx_fetch_error_emit_simple(FILE* stream, MiraErrorClass class_id, const char* summary, const char* url, const char* path, int curl_code, int error_number);
void bx_fetch_error_emit_structured(FILE* stream, const MiraStructuredError* error);

#endif  // BX_FETCH_ERROR_H
