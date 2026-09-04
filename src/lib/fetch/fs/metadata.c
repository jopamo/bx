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

bool metadata_is_empty(const MiraMetadata* meta) {
    if (!meta)
        return true;

    return (!meta->etag || meta->etag[0] == '\0') && (!meta->last_modified || meta->last_modified[0] == '\0') && (!meta->origin_url || meta->origin_url[0] == '\0') &&
           (!meta->redirect_target || meta->redirect_target[0] == '\0') && (!meta->local_path || meta->local_path[0] == '\0');
}

int metadata_write_stream(FILE* f, const MiraMetadata* meta) {
    if (!f || !meta) {
        errno = EINVAL;
        return -1;
    }

    char* origin_url = NULL;
    char* redirect_target = NULL;
    if (meta->origin_url && meta->origin_url[0] != '\0') {
        origin_url = mira_url_display_safe(meta->origin_url);
        if (!origin_url)
            return -1;
    }
    if (meta->redirect_target && meta->redirect_target[0] != '\0') {
        redirect_target = mira_url_display_safe(meta->redirect_target);
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

void metadata_clear(MiraMetadata* meta) {
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

int metadata_load(const char* output_path, MiraMetadata* meta) {
    if (!output_path || !meta)
        return -1;

    metadata_clear(meta);

    char* path = metadata_path_for(output_path);
    if (!path)
        return -1;

    int fd = writer_open_existing_file(path);
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

    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "etag=", 5) == 0) {
            if (set_string(&meta->etag, line + 5) != 0) {
                fclose(f);
                metadata_clear(meta);
                return -1;
            }
        }
        else if (strncmp(line, "last_modified=", 14) == 0) {
            if (set_string(&meta->last_modified, line + 14) != 0) {
                fclose(f);
                metadata_clear(meta);
                return -1;
            }
        }
        else if (strncmp(line, "origin_url=", 11) == 0) {
            if (set_string(&meta->origin_url, line + 11) != 0) {
                fclose(f);
                metadata_clear(meta);
                return -1;
            }
        }
        else if (strncmp(line, "redirect_target=", 16) == 0) {
            if (set_string(&meta->redirect_target, line + 16) != 0) {
                fclose(f);
                metadata_clear(meta);
                return -1;
            }
        }
        else if (strncmp(line, "local_path=", 11) == 0) {
            if (set_string(&meta->local_path, line + 11) != 0) {
                fclose(f);
                metadata_clear(meta);
                return -1;
            }
        }
    }

    if (ferror(f)) {
        fclose(f);
        metadata_clear(meta);
        return -1;
    }

    fclose(f);
    return 0;
}

int metadata_save(const char* output_path, const MiraMetadata* meta) {
    if (!output_path || !meta)
        return -1;

    char* path = metadata_path_for(output_path);
    if (!path)
        return -1;

    if (metadata_is_empty(meta)) {
        int rc = writer_unlink_file(path);
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
    int serialize_rc = metadata_write_stream(stream, meta);
    if (fclose(stream) != 0)
        serialize_rc = -1;
    if (serialize_rc != 0) {
        int serialize_error_number = errno ? errno : EIO;
        free(serialized);
        free(path);
        errno = serialize_error_number;
        return -1;
    }

    Writer* writer = writer_open_with_options(path, WRITER_CREATE, 0, false);
    if (!writer) {
        free(serialized);
        free(path);
        return -1;
    }
    if (writer_write(writer, serialized, serialized_len) != 0) {
        int write_error_number = errno ? errno : EIO;
        writer_abort(writer);
        free(serialized);
        free(path);
        errno = write_error_number;
        return -1;
    }
    free(serialized);
    free(path);
    return writer_close(writer);
}
