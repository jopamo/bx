#ifndef MIRA_ERROR_H
#define MIRA_ERROR_H

/* MIRA_HEADER_OWNER: util */
/* MIRA_HEADER_CONSUMERS: util, entry, cli, core, runtime, policy, net, fs, store, crawl */

/*
 * Layering contract:
 * - Central error taxonomy shared across layers.
 * - Lower layers emit typed failures; entry/core map them to user-visible exits.
 *
 * Ownership and lifetime:
 * - Error enums are value types.
 * - MiraStructuredError string fields are borrowed pointers; emitters do not
 *   copy or free them.
 * - Pointers supplied to mira_error_emit_structured() must remain valid for the
 *   duration of the call.
 */

#include <stdbool.h>
#include <stdio.h>

typedef enum {
    MIRA_OK = 0,
    MIRA_ERROR_INVALID_ARGUMENT,
    MIRA_ERROR_MEMORY,
    MIRA_ERROR_IO,
    MIRA_ERROR_NETWORK,
    MIRA_ERROR_HTTP,
    MIRA_ERROR_SSL,
    MIRA_ERROR_TIMEOUT,
    MIRA_ERROR_CANCELLED,
    MIRA_ERROR_UNSUPPORTED,
    MIRA_ERROR_INTERNAL,
    MIRA_ERROR_RESOURCE_LIMIT,
} MiraError;

const char *mira_error_string(MiraError err);

typedef enum {
    MIRA_ERROR_CLASS_PARSE = 0,
    MIRA_ERROR_CLASS_POLICY,
    MIRA_ERROR_CLASS_HTTP,
    MIRA_ERROR_CLASS_TLS,
    MIRA_ERROR_CLASS_CURL_TRANSPORT,
    MIRA_ERROR_CLASS_FILESYSTEM,
    MIRA_ERROR_CLASS_STATE_STORE,
    MIRA_ERROR_CLASS_INTERNAL,
} MiraErrorClass;

typedef enum {
    MIRA_TRANSPORT_ERROR_NONE = 0,
    MIRA_TRANSPORT_ERROR_NETWORK,
    MIRA_TRANSPORT_ERROR_PROTOCOL,
    MIRA_TRANSPORT_ERROR_TLS_RETRYABLE,
    MIRA_TRANSPORT_ERROR_TLS_FATAL,
} MiraTransportErrorKind;

typedef struct {
    MiraErrorClass class_id;
    const char *summary;
    const char *url;
    const char *path;
    int http_status;
    int curl_code;
    int error_number;
    bool retryable;
    int attempt;
    int max_attempts;
} MiraStructuredError;

const char *mira_error_class_string(MiraErrorClass class_id);
MiraStructuredError mira_error_make_simple(MiraErrorClass class_id, const char *summary,
                                           const char *url, const char *path,
                                           int curl_code, int error_number);
void mira_error_emit_simple(FILE *stream, MiraErrorClass class_id, const char *summary,
                            const char *url, const char *path, int curl_code,
                            int error_number);
void mira_error_emit_structured(FILE *stream, const MiraStructuredError *error);

#endif // MIRA_ERROR_H
