#include "lib/fetch/exit_code.h"

static const MiraExitCodeInfo k_exit_code_table[] = {
    {
        .code = MIRA_EXIT_SUCCESS,
        .label = "success",
        .description = "completed without terminal errors",
    },
    {
        .code = MIRA_EXIT_PARSE_OR_CONFIG,
        .label = "parse-config",
        .description = "invalid CLI syntax or configuration",
    },
    {
        .code = MIRA_EXIT_FILE_IO,
        .label = "file-io",
        .description = "filesystem or persistent-state read/write failure",
    },
    {
        .code = MIRA_EXIT_NETWORK,
        .label = "network",
        .description = "transport, scheduler, or runtime network infrastructure failure",
    },
    {
        .code = MIRA_EXIT_SSL,
        .label = "tls",
        .description = "TLS handshake, certificate, or pin verification failure",
    },
    {
        .code = MIRA_EXIT_AUTH,
        .label = "auth",
        .description = "authentication challenge or credential failure",
    },
    {
        .code = MIRA_EXIT_PROTOCOL,
        .label = "protocol",
        .description = "protocol, URL, or redirect semantics failure",
    },
    {
        .code = MIRA_EXIT_SERVER,
        .label = "server",
        .description = "HTTP server returned a terminal 4xx/5xx response",
    },
    {
        .code = MIRA_EXIT_POLICY,
        .label = "policy",
        .description = "policy subsystem initialization or enforcement failure",
    },
};

const MiraExitCodeInfo* mira_exit_code_table(size_t* count) {
    if (count) {
        *count = sizeof(k_exit_code_table) / sizeof(k_exit_code_table[0]);
    }
    return k_exit_code_table;
}

const MiraExitCodeInfo* mira_exit_code_info(int code) {
    size_t count = 0;
    const MiraExitCodeInfo* table = mira_exit_code_table(&count);
    for (size_t i = 0; i < count; i++) {
        if ((int)table[i].code == code) {
            return &table[i];
        }
    }
    return NULL;
}

bool mira_exit_code_is_assigned(int code) {
    return mira_exit_code_info(code) != NULL;
}

int mira_exit_code_for_error_class(MiraErrorClass class_id, int http_status) {
    switch (class_id) {
        case MIRA_ERROR_CLASS_PARSE:
            return MIRA_EXIT_PARSE_OR_CONFIG;
        case MIRA_ERROR_CLASS_POLICY:
            return MIRA_EXIT_POLICY;
        case MIRA_ERROR_CLASS_FILESYSTEM:
        case MIRA_ERROR_CLASS_STATE_STORE:
            return MIRA_EXIT_FILE_IO;
        case MIRA_ERROR_CLASS_TLS:
            return MIRA_EXIT_SSL;
        case MIRA_ERROR_CLASS_HTTP:
            if (http_status == 401 || http_status == 407) {
                return MIRA_EXIT_AUTH;
            }
            return MIRA_EXIT_SERVER;
        case MIRA_ERROR_CLASS_CURL_TRANSPORT:
            return MIRA_EXIT_NETWORK;
        case MIRA_ERROR_CLASS_INTERNAL:
        default:
            return MIRA_EXIT_NETWORK;
    }
}

int mira_exit_code_for_transfer_failure(int http_status, MiraTransportErrorKind transport_kind, MiraError result) {
    if (http_status == 401 || http_status == 407) {
        return MIRA_EXIT_AUTH;
    }
    if (http_status >= 400 && http_status < 600) {
        return MIRA_EXIT_SERVER;
    }

    switch (result) {
        case MIRA_OK:
        case MIRA_ERROR_CANCELLED:
            return MIRA_EXIT_SUCCESS;
        case MIRA_ERROR_IO:
            return MIRA_EXIT_FILE_IO;
        case MIRA_ERROR_SSL:
            return MIRA_EXIT_SSL;
        case MIRA_ERROR_RESOURCE_LIMIT:
            return MIRA_EXIT_POLICY;
        case MIRA_ERROR_UNSUPPORTED:
        case MIRA_ERROR_HTTP:
            return MIRA_EXIT_PROTOCOL;
        case MIRA_ERROR_TIMEOUT:
        case MIRA_ERROR_NETWORK:
            break;
        default:
            return MIRA_EXIT_NETWORK;
    }

    switch (transport_kind) {
        case MIRA_TRANSPORT_ERROR_TLS_RETRYABLE:
        case MIRA_TRANSPORT_ERROR_TLS_FATAL:
            return MIRA_EXIT_SSL;
        case MIRA_TRANSPORT_ERROR_PROTOCOL:
            return MIRA_EXIT_PROTOCOL;
        case MIRA_TRANSPORT_ERROR_NONE:
        case MIRA_TRANSPORT_ERROR_NETWORK:
        default:
            return MIRA_EXIT_NETWORK;
    }
}

MiraErrorClass mira_error_class_for_exit_code(int exit_code) {
    switch (exit_code) {
        case MIRA_EXIT_PARSE_OR_CONFIG:
            return MIRA_ERROR_CLASS_PARSE;
        case MIRA_EXIT_POLICY:
            return MIRA_ERROR_CLASS_POLICY;
        case MIRA_EXIT_FILE_IO:
            return MIRA_ERROR_CLASS_FILESYSTEM;
        case MIRA_EXIT_SSL:
            return MIRA_ERROR_CLASS_TLS;
        case MIRA_EXIT_AUTH:
        case MIRA_EXIT_SERVER:
            return MIRA_ERROR_CLASS_HTTP;
        case MIRA_EXIT_PROTOCOL:
        case MIRA_EXIT_NETWORK:
            return MIRA_ERROR_CLASS_CURL_TRANSPORT;
        default:
            return MIRA_ERROR_CLASS_INTERNAL;
    }
}
