#include "lib/fetch/exit_code.h"

static const MiraExitCodeInfo k_exit_code_table[] = {
    {
        .code = BX_FETCH_EXIT_SUCCESS,
        .label = "success",
        .description = "completed without terminal errors",
    },
    {
        .code = BX_FETCH_EXIT_PARSE_OR_CONFIG,
        .label = "parse-config",
        .description = "invalid CLI syntax or configuration",
    },
    {
        .code = BX_FETCH_EXIT_FILE_IO,
        .label = "file-io",
        .description = "filesystem or persistent-state read/write failure",
    },
    {
        .code = BX_FETCH_EXIT_NETWORK,
        .label = "network",
        .description = "transport, scheduler, or runtime network infrastructure failure",
    },
    {
        .code = BX_FETCH_EXIT_SSL,
        .label = "tls",
        .description = "TLS handshake, certificate, or pin verification failure",
    },
    {
        .code = BX_FETCH_EXIT_AUTH,
        .label = "auth",
        .description = "authentication challenge or credential failure",
    },
    {
        .code = BX_FETCH_EXIT_PROTOCOL,
        .label = "protocol",
        .description = "protocol, URL, or redirect semantics failure",
    },
    {
        .code = BX_FETCH_EXIT_SERVER,
        .label = "server",
        .description = "HTTP server returned a terminal 4xx/5xx response",
    },
    {
        .code = BX_FETCH_EXIT_POLICY,
        .label = "policy",
        .description = "policy subsystem initialization or enforcement failure",
    },
};

const MiraExitCodeInfo* bx_fetch_exit_code_table(size_t* count) {
    if (count) {
        *count = sizeof(k_exit_code_table) / sizeof(k_exit_code_table[0]);
    }
    return k_exit_code_table;
}

const MiraExitCodeInfo* bx_fetch_exit_code_info(int code) {
    size_t count = 0;
    const MiraExitCodeInfo* table = bx_fetch_exit_code_table(&count);
    for (size_t i = 0; i < count; i++) {
        if ((int)table[i].code == code) {
            return &table[i];
        }
    }
    return NULL;
}

bool bx_fetch_exit_code_is_assigned(int code) {
    return bx_fetch_exit_code_info(code) != NULL;
}

int bx_fetch_exit_code_for_error_class(MiraErrorClass class_id, int http_status) {
    switch (class_id) {
        case BX_FETCH_ERROR_CLASS_PARSE:
            return BX_FETCH_EXIT_PARSE_OR_CONFIG;
        case BX_FETCH_ERROR_CLASS_POLICY:
            return BX_FETCH_EXIT_POLICY;
        case BX_FETCH_ERROR_CLASS_FILESYSTEM:
        case BX_FETCH_ERROR_CLASS_STATE_STORE:
            return BX_FETCH_EXIT_FILE_IO;
        case BX_FETCH_ERROR_CLASS_TLS:
            return BX_FETCH_EXIT_SSL;
        case BX_FETCH_ERROR_CLASS_HTTP:
            if (http_status == 401 || http_status == 407) {
                return BX_FETCH_EXIT_AUTH;
            }
            return BX_FETCH_EXIT_SERVER;
        case BX_FETCH_ERROR_CLASS_CURL_TRANSPORT:
            return BX_FETCH_EXIT_NETWORK;
        case BX_FETCH_ERROR_CLASS_INTERNAL:
        default:
            return BX_FETCH_EXIT_NETWORK;
    }
}

int bx_fetch_exit_code_for_transfer_failure(int http_status, MiraTransportErrorKind transport_kind, MiraError result) {
    if (http_status == 401 || http_status == 407) {
        return BX_FETCH_EXIT_AUTH;
    }
    if (http_status >= 400 && http_status < 600) {
        return BX_FETCH_EXIT_SERVER;
    }

    switch (result) {
        case BX_FETCH_OK:
        case BX_FETCH_ERROR_CANCELLED:
            return BX_FETCH_EXIT_SUCCESS;
        case BX_FETCH_ERROR_IO:
            return BX_FETCH_EXIT_FILE_IO;
        case BX_FETCH_ERROR_SSL:
            return BX_FETCH_EXIT_SSL;
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
            return BX_FETCH_EXIT_POLICY;
        case BX_FETCH_ERROR_UNSUPPORTED:
        case BX_FETCH_ERROR_HTTP:
            return BX_FETCH_EXIT_PROTOCOL;
        case BX_FETCH_ERROR_TIMEOUT:
        case BX_FETCH_ERROR_NETWORK:
            break;
        default:
            return BX_FETCH_EXIT_NETWORK;
    }

    switch (transport_kind) {
        case BX_FETCH_TRANSPORT_ERROR_TLS_RETRYABLE:
        case BX_FETCH_TRANSPORT_ERROR_TLS_FATAL:
            return BX_FETCH_EXIT_SSL;
        case BX_FETCH_TRANSPORT_ERROR_PROTOCOL:
            return BX_FETCH_EXIT_PROTOCOL;
        case BX_FETCH_TRANSPORT_ERROR_NONE:
        case BX_FETCH_TRANSPORT_ERROR_NETWORK:
        default:
            return BX_FETCH_EXIT_NETWORK;
    }
}

MiraErrorClass bx_fetch_error_class_for_exit_code(int exit_code) {
    switch (exit_code) {
        case BX_FETCH_EXIT_PARSE_OR_CONFIG:
            return BX_FETCH_ERROR_CLASS_PARSE;
        case BX_FETCH_EXIT_POLICY:
            return BX_FETCH_ERROR_CLASS_POLICY;
        case BX_FETCH_EXIT_FILE_IO:
            return BX_FETCH_ERROR_CLASS_FILESYSTEM;
        case BX_FETCH_EXIT_SSL:
            return BX_FETCH_ERROR_CLASS_TLS;
        case BX_FETCH_EXIT_AUTH:
        case BX_FETCH_EXIT_SERVER:
            return BX_FETCH_ERROR_CLASS_HTTP;
        case BX_FETCH_EXIT_PROTOCOL:
        case BX_FETCH_EXIT_NETWORK:
            return BX_FETCH_ERROR_CLASS_CURL_TRANSPORT;
        default:
            return BX_FETCH_ERROR_CLASS_INTERNAL;
    }
}
