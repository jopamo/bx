#define _GNU_SOURCE
#include "lib/fetch/metadata.h"
#include "lib/fetch/url.h"
#include "lib/fetch/writer.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int set_string(char** dest, const char* src) {
    char* copy = src ? strdup(src) : NULL;
    if (src && !copy)
        return -1;

    free(*dest);
    *dest = copy;
    return 0;
}

static char* metadata_path_for(const char* output_path) {
    size_t len = strlen(output_path) + strlen(".mira.meta") + 1;
    char* path = malloc(len);
    if (!path)
        return NULL;
    snprintf(path, len, "%s.mira.meta", output_path);
    return path;
}

bool bx_fetch_metadata_is_empty(const BxFetchMetadata* meta) {
    if (!meta)
        return true;

    return (!meta->etag || meta->etag[0] == '\0') && (!meta->last_modified || meta->last_modified[0] == '\0') && (!meta->origin_url || meta->origin_url[0] == '\0') &&
           (!meta->redirect_target || meta->redirect_target[0] == '\0') && (!meta->local_path || meta->local_path[0] == '\0');
}

int bx_fetch_metadata_write_stream(FILE* f, const BxFetchMetadata* meta) {
    if (!f || !meta) {
        errno = EINVAL;
        return -1;
    }

    char* origin_url = NULL;
    char* redirect_target = NULL;
    if (meta->origin_url && meta->origin_url[0] != '\0') {
        origin_url = bx_fetch_url_display_safe(meta->origin_url);
        if (!origin_url)
            return -1;
    }
    if (meta->redirect_target && meta->redirect_target[0] != '\0') {
        redirect_target = bx_fetch_url_display_safe(meta->redirect_target);
        if (!redirect_target) {
            free(origin_url);
            return -1;
        }
    }

    if (meta->etag && meta->etag[0] != '\0' && fprintf(f, "etag=%s\n", meta->etag) < 0) {
        goto fail;
    }
    if (meta->last_modified && meta->last_modified[0] != '\0' && fprintf(f, "last_modified=%s\n", meta->last_modified) < 0) {
        goto fail;
    }
    if (origin_url && fprintf(f, "origin_url=%s\n", origin_url) < 0) {
        goto fail;
    }
    if (redirect_target && fprintf(f, "redirect_target=%s\n", redirect_target) < 0) {
        goto fail;
    }
    if (meta->local_path && meta->local_path[0] != '\0' && fprintf(f, "local_path=%s\n", meta->local_path) < 0) {
        goto fail;
    }

    free(origin_url);
    free(redirect_target);
    return 0;

fail:
    free(origin_url);
    free(redirect_target);
    return -1;
}

void bx_fetch_metadata_clear(BxFetchMetadata* meta) {
    if (!meta)
        return;
    free(meta->etag);
    free(meta->last_modified);
    free(meta->origin_url);
    free(meta->redirect_target);
    free(meta->local_path);
    meta->etag = NULL;
    meta->last_modified = NULL;
    meta->origin_url = NULL;
    meta->redirect_target = NULL;
    meta->local_path = NULL;
}

static int apply_metadata_line(BxFetchMetadata* meta, const char* line) {
    if (strncmp(line, "etag=", 5) == 0)
        return set_string(&meta->etag, line + 5);
    if (strncmp(line, "last_modified=", 14) == 0)
        return set_string(&meta->last_modified, line + 14);
    if (strncmp(line, "origin_url=", 11) == 0)
        return set_string(&meta->origin_url, line + 11);
    if (strncmp(line, "redirect_target=", 16) == 0)
        return set_string(&meta->redirect_target, line + 16);
    if (strncmp(line, "local_path=", 11) == 0)
        return set_string(&meta->local_path, line + 11);
    return 0;
}

int bx_fetch_metadata_read_stream(FILE* f, BxFetchMetadata* meta) {
    if (!f || !meta) {
        errno = EINVAL;
        return -1;
    }
    bx_fetch_metadata_clear(meta);

    size_t total_bytes = 0;
    size_t field_count = 0;
    for (;;) {
        char line[BX_FETCH_METADATA_LINE_MAX_BYTES + 1u];
        size_t length = 0;
        bool saw_byte = false;
        bool reached_eof = false;

        for (;;) {
            int byte = fgetc(f);
            if (byte == EOF) {
                if (ferror(f)) {
                    if (errno == 0)
                        errno = EIO;
                    goto fail;
                }
                reached_eof = true;
                break;
            }
            saw_byte = true;
            if (total_bytes >= BX_FETCH_METADATA_FILE_MAX_BYTES) {
                errno = EFBIG;
                goto fail;
            }
            total_bytes++;
            if (byte == '\n')
                break;
            if (byte == '\0') {
                errno = EINVAL;
                goto fail;
            }
            if (length >= BX_FETCH_METADATA_LINE_MAX_BYTES) {
                errno = EFBIG;
                goto fail;
            }
            line[length++] = (char)byte;
        }

        if (!saw_byte && reached_eof)
            break;
        if (length > 0 && line[length - 1] == '\r')
            length--;
        if (memchr(line, '\r', length)) {
            errno = EINVAL;
            goto fail;
        }
        line[length] = '\0';

        if (field_count >= BX_FETCH_METADATA_MAX_FIELDS) {
            errno = EFBIG;
            goto fail;
        }
        field_count++;
        if (apply_metadata_line(meta, line) != 0)
            goto fail;
        if (reached_eof)
            break;
    }
    return 0;

fail:
    bx_fetch_metadata_clear(meta);
    return -1;
}

int bx_fetch_metadata_load(const char* output_path, BxFetchMetadata* meta) {
    if (!output_path || !meta)
        return -1;

    bx_fetch_metadata_clear(meta);

    char* path = metadata_path_for(output_path);
    if (!path)
        return -1;

    int fd = bx_fetch_writer_open_existing_file(path);
    if (fd == -1) {
        int open_error_number = errno;
        free(path);
        errno = open_error_number;
        return (open_error_number == ENOENT) ? 0 : -1;
    }
    free(path);

    FILE* f = fdopen(fd, "r");
    if (!f) {
        int open_error_number = errno;
        close(fd);
        errno = open_error_number;
        return -1;
    }

    int result = bx_fetch_metadata_read_stream(f, meta);
    int error_number = errno;
    if (fclose(f) != 0 && result == 0) {
        result = -1;
        error_number = errno;
        bx_fetch_metadata_clear(meta);
    }
    if (result != 0)
        errno = error_number;
    return result;
}

int bx_fetch_metadata_save(const char* output_path, const BxFetchMetadata* meta) {
    if (!output_path || !meta)
        return -1;

    char* path = metadata_path_for(output_path);
    if (!path)
        return -1;

    if (bx_fetch_metadata_is_empty(meta)) {
        int rc = bx_fetch_writer_unlink_file(path);
        int unlink_error_number = errno;
        free(path);
        errno = unlink_error_number;
        return (rc == 0 || unlink_error_number == ENOENT) ? 0 : -1;
    }

    char* serialized = NULL;
    size_t serialized_len = 0;
    FILE* stream = open_memstream(&serialized, &serialized_len);
    if (!stream) {
        free(path);
        return -1;
    }
    int serialize_rc = bx_fetch_metadata_write_stream(stream, meta);
    if (fclose(stream) != 0)
        serialize_rc = -1;
    if (serialize_rc != 0) {
        int serialize_error_number = errno ? errno : EIO;
        free(serialized);
        free(path);
        errno = serialize_error_number;
        return -1;
    }

    BxFetchWriter* writer = bx_fetch_writer_open_with_options(path, WRITER_CREATE, 0, false);
    if (!writer) {
        free(serialized);
        free(path);
        return -1;
    }
    if (bx_fetch_writer_write(writer, serialized, serialized_len) != 0) {
        int write_error_number = errno ? errno : EIO;
        bx_fetch_writer_abort(writer);
        free(serialized);
        free(path);
        errno = write_error_number;
        return -1;
    }
    free(serialized);
    free(path);
    return bx_fetch_writer_close(writer);
}
