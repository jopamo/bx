#include "lib/fetch/error.h"
#include <ctype.h>
#include <stdio.h>

static void json_write_escaped_string(FILE* stream, const char* value) {
    fputc('"', stream);
    if (value) {
        for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; p++) {
            unsigned char ch = *p;
            switch (ch) {
                case '"':
                    fputs("\\\"", stream);
                    break;
                case '\\':
                    fputs("\\\\", stream);
                    break;
                case '\b':
                    fputs("\\b", stream);
                    break;
                case '\f':
                    fputs("\\f", stream);
                    break;
                case '\n':
                    fputs("\\n", stream);
                    break;
                case '\r':
                    fputs("\\r", stream);
                    break;
                case '\t':
                    fputs("\\t", stream);
                    break;
                default:
                    if (!isprint(ch)) {
                        fprintf(stream, "\\u%04x", (unsigned int)ch);
                    }
                    else {
                        fputc((int)ch, stream);
                    }
                    break;
            }
        }
    }
    fputc('"', stream);
}

const char* bx_fetch_error_class_string(BxFetchErrorClass class_id) {
    switch (class_id) {
        case BX_FETCH_ERROR_CLASS_PARSE:
            return "parse";
        case BX_FETCH_ERROR_CLASS_POLICY:
            return "policy";
        case BX_FETCH_ERROR_CLASS_HTTP:
            return "http";
        case BX_FETCH_ERROR_CLASS_TLS:
            return "tls";
        case BX_FETCH_ERROR_CLASS_CURL_TRANSPORT:
            return "curl-transport";
        case BX_FETCH_ERROR_CLASS_FILESYSTEM:
            return "filesystem";
        case BX_FETCH_ERROR_CLASS_STATE_STORE:
            return "state-store";
        case BX_FETCH_ERROR_CLASS_INTERNAL:
            return "internal";
        default:
            return "internal";
    }
}

static void json_write_optional_string(FILE* stream, const char* value) {
    if (value && value[0] != '\0') {
        json_write_escaped_string(stream, value);
    }
    else {
        fputs("null", stream);
    }
}

static void json_write_optional_int(FILE* stream, int value) {
    if (value >= 0) {
        fprintf(stream, "%d", value);
    }
    else {
        fputs("null", stream);
    }
}

const char* bx_fetch_error_string(BxFetchError err) {
    switch (err) {
        case BX_FETCH_OK:
            return "Success";
        case BX_FETCH_ERROR_INVALID_ARGUMENT:
            return "Invalid argument";
        case BX_FETCH_ERROR_MEMORY:
            return "Out of memory";
        case BX_FETCH_ERROR_IO:
            return "I/O error";
        case BX_FETCH_ERROR_NETWORK:
            return "Network error";
        case BX_FETCH_ERROR_HTTP:
            return "HTTP error";
        case BX_FETCH_ERROR_SSL:
            return "SSL/TLS error";
        case BX_FETCH_ERROR_TIMEOUT:
            return "Timeout";
        case BX_FETCH_ERROR_CANCELLED:
            return "Operation cancelled";
        case BX_FETCH_ERROR_UNSUPPORTED:
            return "Unsupported operation";
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
            return "Resource limit exceeded";
        case BX_FETCH_ERROR_INTERNAL:
            return "Internal error";
        default:
            return "Unknown error";
    }
}

BxFetchStructuredError bx_fetch_error_make_simple(BxFetchErrorClass class_id, const char* summary, const char* url, const char* path, int curl_code, int error_number) {
    BxFetchStructuredError error = {
        .class_id = class_id,
        .summary = summary,
        .url = url,
        .path = path,
        .http_status = -1,
        .curl_code = curl_code,
        .error_number = error_number,
        .retryable = false,
        .attempt = -1,
        .max_attempts = -1,
    };
    return error;
}

void bx_fetch_error_emit_simple(FILE* stream, BxFetchErrorClass class_id, const char* summary, const char* url, const char* path, int curl_code, int error_number) {
    BxFetchStructuredError error = bx_fetch_error_make_simple(class_id, summary, url, path, curl_code, error_number);
    bx_fetch_error_emit_structured(stream, &error);
}

void bx_fetch_error_emit_structured(FILE* stream, const BxFetchStructuredError* error) {
    if (!error)
        return;
    if (!stream)
        stream = stderr;

    fputs("{\"schema_version\":1,\"class\":", stream);
    json_write_escaped_string(stream, bx_fetch_error_class_string(error->class_id));
    fputs(",\"summary\":", stream);
    json_write_optional_string(stream, error->summary);
    fputs(",\"url\":", stream);
    json_write_optional_string(stream, error->url);
    fputs(",\"path\":", stream);
    json_write_optional_string(stream, error->path);
    fputs(",\"http_status\":", stream);
    json_write_optional_int(stream, error->http_status);
    fputs(",\"curl_code\":", stream);
    json_write_optional_int(stream, error->curl_code);
    fputs(",\"errno\":", stream);
    json_write_optional_int(stream, error->error_number);
    fputs(",\"retryable\":", stream);
    fputs(error->retryable ? "true" : "false", stream);
    fputs(",\"attempt\":", stream);
    json_write_optional_int(stream, error->attempt);
    fputs(",\"max_attempts\":", stream);
    json_write_optional_int(stream, error->max_attempts);
    fputs("}\n", stream);
}
