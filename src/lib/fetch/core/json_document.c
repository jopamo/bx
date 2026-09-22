#define _GNU_SOURCE
#include "lib/fetch/json_document.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/writer.h"
#include "lib/jq/filter.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int read_bounded_json(const char* path, size_t max_bytes, char** data_out, size_t* size_out) {
    *data_out = NULL;
    *size_out = 0;
    int fd = bx_fetch_writer_open_existing_file(path);
    if (fd == -1)
        return -1;

    struct stat status;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        (uintmax_t)status.st_size > max_bytes ||
        (uintmax_t)status.st_size > INT_MAX) {
        int error_number = errno ? errno : EFBIG;
        close(fd);
        errno = error_number;
        return -1;
    }

    size_t size = (size_t)status.st_size;
    char* data = malloc(size + 1u);
    if (!data) {
        close(fd);
        return -1;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            int error_number = count == 0 ? EIO : errno;
            free(data);
            close(fd);
            errno = error_number;
            return -1;
        }
        offset += (size_t)count;
    }
    if (close(fd) != 0) {
        free(data);
        return -1;
    }
    data[size] = '\0';
    *data_out = data;
    *size_out = size;
    return 0;
}

struct BxFetchJsonDocument {
    char directory[32];
    char path[48];
};

BxFetchJsonDocument* bx_fetch_json_document_new(void) {
    BxFetchJsonDocument* document = calloc(1, sizeof(*document));
    if (!document)
        return NULL;
    strcpy(document->directory, "/tmp/bx-json.XXXXXX");
    if (!mkdtemp(document->directory)) {
        free(document);
        return NULL;
    }
    snprintf(document->path, sizeof(document->path), "%s/response", document->directory);
    return document;
}

const char* bx_fetch_json_document_path(const BxFetchJsonDocument* document) {
    return document->path;
}

int bx_fetch_json_document_free(BxFetchJsonDocument* document) {
    if (!document)
        return 0;
    int result = 0;
    if (unlink(document->path) != 0 && errno != ENOENT)
        result = -1;
    if (rmdir(document->directory) != 0)
        result = -1;
    free(document);
    return result;
}

int bx_fetch_json_document_read(const BxFetchJsonDocument* document, size_t max_bytes, jv* value) {
    char* data = NULL;
    size_t length = 0;
    *value = jv_invalid();
    if (read_bounded_json(document->path, max_bytes, &data, &length) != 0)
        return BX_FETCH_EXIT_FILE_IO;
    *value = jv_parse_sized(data, (int)length);
    free(data);
    return jv_is_valid(*value) ? 0 : BX_FETCH_EXIT_PROTOCOL;
}

static int json_output(void* userdata, jv value) {
    BxFetchWriter* writer = userdata;
    jv rendered = jv_dump_string(jv_copy(value), 0);
    if (jv_get_kind(rendered) != JV_KIND_STRING) {
        jv_free(rendered);
        return -1;
    }
    int result = bx_fetch_writer_write(writer, jv_string_value(rendered),
        (size_t)jv_string_length_bytes(jv_copy(rendered)));
    jv_free(rendered);
    if (result == 0)
        result = bx_fetch_writer_write(writer, "\n", 1);
    return result == BX_FETCH_WRITER_WRITE_OK ? 0 : -1;
}

int bx_fetch_json_document_publish(const BxFetchJsonDocument* document, size_t max_bytes) {
    char* data = NULL;
    size_t length = 0;
    if (read_bounded_json(document->path, max_bytes, &data, &length) != 0)
        return BX_FETCH_EXIT_FILE_IO;
    jv value = jv_parse_sized(data, (int)length);
    bool valid = jv_is_valid(value);
    jv_free(value);
    if (!valid) {
        free(data);
        return BX_FETCH_EXIT_PROTOCOL;
    }
    BxFetchWriter* writer = bx_fetch_writer_open("-", WRITER_CREATE);
    if (!writer || bx_fetch_writer_stage_stdout(writer, max_bytes) != 0 ||
        bx_fetch_writer_write(writer, data, length) != BX_FETCH_WRITER_WRITE_OK) {
        free(data);
        bx_fetch_writer_abort(writer);
        return BX_FETCH_EXIT_FILE_IO;
    }
    free(data);
    return bx_fetch_writer_close(writer) == 0 ? 0 : BX_FETCH_EXIT_FILE_IO;
}

int bx_fetch_json_print(jv value, const char* program, size_t max_bytes) {
    BxFetchWriter* writer = bx_fetch_writer_open("-", WRITER_CREATE);
    if (!writer || bx_fetch_writer_stage_stdout(writer, max_bytes) != 0) {
        bx_fetch_writer_abort(writer);
        jv_free(value);
        return BX_FETCH_EXIT_FILE_IO;
    }
    int result = 0;
    if (program) {
        BxJqFilterResult filter_result;
        bx_jq_filter_result_init(&filter_result);
        BxJqFilterStatus status = bx_jq_filter(value, program, json_output, writer, &filter_result);
        if (status != BX_JQ_FILTER_OK && !(status == BX_JQ_FILTER_HALTED && filter_result.halt_code == 0)) {
            result = status == BX_JQ_FILTER_COMPILE_ERROR ? BX_FETCH_EXIT_PARSE_OR_CONFIG : BX_FETCH_EXIT_PROTOCOL;
        }
        bx_jq_filter_result_clear(&filter_result);
    } else {
        if (json_output(writer, value) != 0)
            result = BX_FETCH_EXIT_FILE_IO;
        jv_free(value);
    }
    if (result != 0) {
        bx_fetch_writer_abort(writer);
        return result;
    }
    return bx_fetch_writer_close(writer) == 0 ? 0 : BX_FETCH_EXIT_FILE_IO;
}
