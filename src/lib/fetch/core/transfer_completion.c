#define _GNU_SOURCE
#include "lib/fetch/transfer_completion.h"
#include "lib/fetch/metadata.h"
#include "lib/fetch/timestamp_policy.h"
#include "lib/fetch/xattr.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum {
    BX_FETCH_CURL_CODE_COULDNT_CONNECT = 7,
};

static bool replace_metadata_string(char** destination, const char* value) {
    if (!destination || !value || value[0] == '\0')
        return true;

    char* copy = strdup(value);
    if (!copy)
        return false;

    free(*destination);
    *destination = copy;
    return true;
}

static int stage_response_metadata(const struct bx_fetch_config* cfg, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer) {
    if (!cfg->download.metadata_sidecars)
        return 0;

    const char* output_path = bx_fetch_writer_get_path(writer);
    if (!output_path || strcmp(output_path, "-") == 0)
        return 0;

    BxFetchMetadata metadata = {0};
    if (bx_fetch_writer_load_original_metadata(writer, &metadata) != 0)
        return -1;

    const BxFetchPreparedUrl* effective_target = bx_fetch_response_effective_target(response);
    const char* request_url = bx_fetch_request_url_for_display(request);
    const char* effective_url = effective_target ? bx_fetch_prepared_url_display(effective_target) : request_url;

    bool complete =
        replace_metadata_string(&metadata.origin_url, request_url) && replace_metadata_string(&metadata.redirect_target, effective_url) && replace_metadata_string(&metadata.local_path, output_path);
    if (complete && cfg->download.timestamping) {
        complete = replace_metadata_string(&metadata.etag, bx_fetch_response_header_value(response, "ETag")) &&
                   replace_metadata_string(&metadata.last_modified, bx_fetch_response_header_value(response, "Last-Modified"));
    }
    if (!complete) {
        int error_number = errno ? errno : ENOMEM;
        bx_fetch_metadata_clear(&metadata);
        errno = error_number;
        return -1;
    }

    int result = bx_fetch_writer_stage_metadata(writer, &metadata);
    int error_number = errno;
    bx_fetch_metadata_clear(&metadata);
    if (result != 0)
        errno = error_number ? error_number : EIO;
    return result;
}

int bx_fetch_transfer_stage_response(const struct bx_fetch_config* cfg, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer) {
    if (!cfg || !request || !response || !writer) {
        errno = EINVAL;
        return -1;
    }
    if (cfg->download.spider)
        return 0;
    if (response->status_code != 200 && response->status_code != 206) {
        errno = EINVAL;
        return -1;
    }

    if (stage_response_metadata(cfg, request, response, writer) != 0)
        return -1;

    const char* output_path = bx_fetch_writer_get_path(writer);
    const char* last_modified = bx_fetch_response_header_value(response, "Last-Modified");
    time_t server_mtime = 0;
    if (bx_fetch_timestamp_should_use_server_time(cfg->download.no_use_server_timestamps, response->status_code, output_path, last_modified, &server_mtime) &&
        bx_fetch_writer_set_mtime(writer, server_mtime) != 0) {
        return -1;
    }

    if (cfg->download.xattr) {
        const char* request_url = bx_fetch_request_url_for_display(request);
        const char* content_type = response->content_type ? response->content_type : bx_fetch_response_header_value(response, "Content-Type");
        int xattr_result = bx_fetch_writer_stage_xattrs(writer, request_url, content_type, bx_fetch_response_header_value(response, "ETag"), last_modified);
        if (xattr_result != BX_FETCH_XATTR_OK) {
            if (xattr_result == BX_FETCH_XATTR_UNSUPPORTED)
                errno = ENOTSUP;
            else if (errno == 0)
                errno = EIO;
            return -1;
        }
    }

    return 0;
}

int bx_fetch_transfer_stage_not_modified(const struct bx_fetch_config* cfg, const BxFetchRequest* request, const BxFetchResponse* response, BxFetchWriter* writer) {
    if (!cfg || !request || !response || !writer || response->status_code != 304) {
        errno = EINVAL;
        return -1;
    }
    if (cfg->download.spider)
        return 0;
    if (cfg->download.xattr) {
        /*
         * Sidecars can be exchanged and rolled back through the retained
         * directory descriptor. Xattrs mutate the existing payload inode and
         * cannot join that transaction. Replacing the inode would violate 304
         * payload identity, so reject the combination before either changes.
         */
        errno = ENOTSUP;
        return -1;
    }
    return stage_response_metadata(cfg, request, response, writer);
}

static bool retryable_io_error_number(int error_number) {
    if (error_number < 0)
        return true;

    switch (error_number) {
        case EINTR:
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case ETIMEDOUT:
        case ENFILE:
        case EMFILE:
            return true;
        default:
            return false;
    }
}

bool bx_fetch_transfer_retryable_hint(const struct bx_fetch_config* cfg, const BxFetchResponse* response, BxFetchError result) {
    int curl_code = response ? response->error_code : 0;
    int error_number = response ? response->error_number : -1;
    BxFetchTransportErrorKind transport_kind = response ? response->transport_error_kind : BX_FETCH_TRANSPORT_ERROR_NONE;

    switch (result) {
        case BX_FETCH_ERROR_SSL:
            return transport_kind == BX_FETCH_TRANSPORT_ERROR_TLS_RETRYABLE;
        case BX_FETCH_ERROR_NETWORK:
            if (error_number == ECONNREFUSED || curl_code == BX_FETCH_CURL_CODE_COULDNT_CONNECT)
                return cfg && cfg->download.retry_connrefused;
            return true;
        case BX_FETCH_ERROR_IO:
            return retryable_io_error_number(error_number);
        case BX_FETCH_ERROR_MEMORY:
        case BX_FETCH_ERROR_INVALID_ARGUMENT:
        case BX_FETCH_ERROR_RESOURCE_LIMIT:
        case BX_FETCH_ERROR_INTERNAL:
            return false;
        default:
            return true;
    }
}
