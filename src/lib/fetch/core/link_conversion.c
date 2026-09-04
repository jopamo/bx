#define _GNU_SOURCE
#include "lib/fetch/crawl/document_internal.h"
#include "lib/fetch/link_conversion.h"
#include "lib/fetch/writer.h"
#include "lib/path_ops.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const struct bx_fetch_config* cfg;
    const BxFetchPublicationState* publication;
    const BxFetchPreparedUrl* base;
    int error_number;
} ConversionRewriteContext;

static bool conversion_path_has_extension(const char* path, const char* extension) {
    const char* actual = bx_path_extension_ptr(path);
    return actual && extension && strcasecmp(actual, extension) == 0;
}

static int conversion_fail(BxFetchLinkConversionOutcome* outcome, BxFetchLinkConversionFailure failure, int error_number) {
    if (outcome) {
        outcome->failure = failure;
        outcome->error_number = error_number;
    }
    errno = error_number;
    return -1;
}

static char* rewrite_published_link(void* userdata, const char* reference) {
    ConversionRewriteContext* context = userdata;
    if (!context || !reference)
        return NULL;

    BxFetchPreparedUrl* target = bx_fetch_prepared_url_resolve(context->base, reference);
    if (!target) {
        if (errno == ENOMEM || errno == EFBIG || errno == EOVERFLOW)
            context->error_number = errno;
        return NULL;
    }
    const char* local_path = bx_fetch_publication_lookup_prepared(context->publication, target, NULL);
    bx_fetch_prepared_url_free(target);
    if (!local_path)
        return NULL;

    const char* rewritten = local_path;
    if (context->cfg->recursive.convert_file_only) {
        const char* basename = bx_path_basename_ptr(local_path);
        if (basename)
            rewritten = basename;
    }

    const char* fragment = strchr(reference, '#');
    size_t rewritten_length = strlen(rewritten);
    size_t fragment_length = fragment ? strlen(fragment) : 0;
    if (rewritten_length > SIZE_MAX - fragment_length - 1u) {
        context->error_number = EOVERFLOW;
        errno = EOVERFLOW;
        return NULL;
    }
    char* result = malloc(rewritten_length + fragment_length + 1u);
    if (!result) {
        context->error_number = ENOMEM;
        return NULL;
    }
    memcpy(result, rewritten, rewritten_length);
    if (fragment_length > 0)
        memcpy(result + rewritten_length, fragment, fragment_length);
    result[rewritten_length + fragment_length] = '\0';
    return result;
}

static int stage_conversion_output(BxFetchWriter* writer, const void* data, size_t length, const struct stat* source_identity, const BxFetchDownloadedFileView* download) {
    if (!writer || (!data && length > 0u)) {
        errno = EINVAL;
        return -1;
    }
    if (source_identity && bx_fetch_writer_require_original_identity(writer, source_identity) != 0) {
        return -1;
    }
    if (bx_fetch_writer_write(writer, data, length) != 0)
        return -1;
    if (source_identity && bx_fetch_writer_preserve_destination_metadata(writer) != 0) {
        return -1;
    }
    if (download && download->has_server_mtime && bx_fetch_writer_set_mtime(writer, download->server_mtime) != 0) {
        return -1;
    }
    return 0;
}

int bx_fetch_document_convert_download(const struct bx_fetch_config* cfg,
                                       const BxFetchPublicationState* publication,
                                       const BxFetchDownloadedFileView* download,
                                       BxFetchLinkConversionOutcome* outcome) {
    if (outcome)
        *outcome = (BxFetchLinkConversionOutcome){0};
    if (!cfg || !publication || !download || !download->url || !download->local_path) {
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_INVALID_ARGUMENT, EINVAL);
    }
    if (!conversion_path_has_extension(download->local_path, ".html") && !conversion_path_has_extension(download->local_path, ".htm")) {
        return 0;
    }

    BxFetchPreparedUrl* base = bx_fetch_url_prepare(download->url);
    if (!base) {
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_BASE_URL, errno ? errno : EINVAL);
    }

    unsigned char* original = NULL;
    size_t original_length = 0;
    struct stat source_identity;
    BxFetchDocumentOutcome document_outcome = {0};
    if (bx_fetch_document_read(download->local_path, &original, &original_length, &source_identity, &document_outcome) != 0) {
        int error_number = errno;
        bx_fetch_prepared_url_free(base);
        if (outcome)
            outcome->document = document_outcome;
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_DOCUMENT, error_number);
    }
    if (memchr(original, '\0', original_length)) {
        free(original);
        bx_fetch_prepared_url_free(base);
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_REWRITE, EINVAL);
    }

    ConversionRewriteContext rewrite = {
        .cfg = cfg,
        .publication = publication,
        .base = base,
    };
    errno = 0;
    char* converted = bx_fetch_html_convert_links(bx_fetch_prepared_url_transport(base), (const char*)original, original_length, rewrite_published_link, &rewrite);
    int rewrite_error = errno;
    bx_fetch_prepared_url_free(base);
    if (!converted) {
        free(original);
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_REWRITE, rewrite_error ? rewrite_error : EINVAL);
    }
    if (rewrite.error_number) {
        free(converted);
        free(original);
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_REWRITE, rewrite.error_number);
    }

    size_t converted_length = strnlen(converted, BX_FETCH_DOCUMENT_MAX_BYTES + 1u);
    if (converted_length > BX_FETCH_DOCUMENT_MAX_BYTES) {
        free(converted);
        free(original);
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_REWRITE, EFBIG);
    }
    if (converted_length == original_length && memcmp(converted, original, original_length) == 0) {
        free(converted);
        free(original);
        if (outcome)
            outcome->status = BX_FETCH_LINK_CONVERSION_UNCHANGED;
        return 0;
    }

    BxFetchWriter* output = bx_fetch_writer_open_with_options(download->local_path, WRITER_CREATE, 0, false);
    if (!output || stage_conversion_output(output, converted, converted_length, &source_identity, download) != 0) {
        int error_number = errno ? errno : EIO;
        if (output)
            bx_fetch_writer_abort(output);
        free(converted);
        free(original);
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_OUTPUT_STAGE, error_number);
    }

    if (cfg->recursive.backup_converted) {
        char* backup_path = NULL;
        if (asprintf(&backup_path, "%s.orig", download->local_path) == -1)
            backup_path = NULL;
        BxFetchWriter* backup = backup_path ? bx_fetch_writer_open_with_options(backup_path, WRITER_CREATE, 0, false) : NULL;
        if (!backup || stage_conversion_output(backup, original, original_length, NULL, NULL) != 0) {
            int error_number = errno ? errno : ENOMEM;
            if (backup)
                bx_fetch_writer_abort(backup);
            bx_fetch_writer_abort(output);
            free(backup_path);
            free(converted);
            free(original);
            return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_BACKUP_STAGE, error_number);
        }
        free(backup_path);
        if (bx_fetch_writer_close(backup) != 0) {
            int error_number = errno ? errno : EIO;
            bx_fetch_writer_abort(output);
            free(converted);
            free(original);
            return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_BACKUP_COMMIT, error_number);
        }
    }

    free(converted);
    free(original);
    if (bx_fetch_writer_close(output) != 0) {
        return conversion_fail(outcome, BX_FETCH_LINK_CONVERSION_FAILURE_OUTPUT_COMMIT, errno ? errno : EIO);
    }
    if (outcome)
        outcome->status = BX_FETCH_LINK_CONVERSION_COMMITTED;
    return 0;
}
