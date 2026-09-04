#define _GNU_SOURCE
#include "lib/fetch/input.h"
#include "lib/fetch/html.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/url.h"
#include "lib/fd_ops.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int input_fail(BxFetchInputOutcome* outcome, BxFetchInputFailureKind kind, size_t line_number, int error_number) {
    if (error_number <= 0)
        error_number = EIO;
    if (outcome) {
        outcome->kind = kind;
        outcome->line_number = line_number;
        outcome->error_number = error_number;
    }
    errno = error_number;
    return -1;
}

static FILE* input_open(const char* path, BxFetchInputOutcome* outcome) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0) {
        input_fail(outcome, BX_FETCH_INPUT_FAILURE_OPEN, 0, errno);
        return NULL;
    }
    FILE* stream = fdopen(fd, "r");
    if (!stream) {
        int error_number = errno;
        bx_fd_cleanup(&fd);
        input_fail(outcome, BX_FETCH_INPUT_FAILURE_OPEN, 0, error_number);
        return NULL;
    }
    return stream;
}

void bx_fetch_input_urls_free(BxFetchInputUrls* urls) {
    if (!urls)
        return;
    for (size_t index = 0; index < urls->count; index++)
        free(urls->urls[index]);
    free(urls->urls);
    *urls = (BxFetchInputUrls){0};
}

static int input_urls_append(BxFetchInputUrls* urls, const char* value, size_t length, size_t reserved_entries, size_t reserved_bytes, size_t line_number, BxFetchInputOutcome* outcome) {
    if (!bx_fetch_resource_can_reserve(reserved_entries + urls->count, reserved_bytes + urls->retained_bytes, 1u, length, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        return input_fail(outcome, BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT, line_number, EFBIG);
    }

    if (urls->count == urls->capacity) {
        size_t next_capacity = urls->capacity ? urls->capacity * 2u : 16u;
        size_t remaining = BX_FETCH_URL_STATE_MAX_ENTRIES - reserved_entries;
        if (next_capacity < urls->capacity || next_capacity > remaining)
            next_capacity = remaining;
        if (next_capacity <= urls->count || next_capacity > SIZE_MAX / sizeof(*urls->urls))
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT, line_number, EFBIG);
        char** grown = realloc(urls->urls, next_capacity * sizeof(*urls->urls));
        if (!grown)
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_MEMORY, line_number, ENOMEM);
        urls->urls = grown;
        urls->capacity = next_capacity;
    }
    urls->urls[urls->count] = malloc(length + 1u);
    if (!urls->urls[urls->count])
        return input_fail(outcome, BX_FETCH_INPUT_FAILURE_MEMORY, line_number, ENOMEM);
    memcpy(urls->urls[urls->count], value, length);
    urls->urls[urls->count][length] = '\0';
    urls->count++;
    urls->retained_bytes += length;
    return 0;
}

static int input_read_line(FILE* stream, char* line, size_t line_number, size_t* total_bytes, size_t* length_out, bool* eof_out, BxFetchInputOutcome* outcome) {
    size_t length = 0;
    *eof_out = false;
    for (;;) {
        errno = 0;
        int byte = fgetc(stream);
        if (byte == EOF) {
            if (ferror(stream))
                return input_fail(outcome, BX_FETCH_INPUT_FAILURE_READ, line_number, errno ? errno : EIO);
            if (length == 0) {
                *eof_out = true;
                return 0;
            }
            break;
        }
        if (*total_bytes >= BX_FETCH_INPUT_FILE_MAX_BYTES)
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_FILE_TOO_LARGE, line_number, EFBIG);
        (*total_bytes)++;
        if (byte == '\n')
            break;
        if (byte == '\0')
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_INVALID_CONTROL, line_number, EINVAL);
        if (length > BX_FETCH_URL_MAX_BYTES)
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_LINE_TOO_LONG, line_number, EFBIG);
        line[length++] = (char)byte;
    }

    if (length > 0 && line[length - 1] == '\r')
        length--;
    if (length > BX_FETCH_URL_MAX_BYTES)
        return input_fail(outcome, BX_FETCH_INPUT_FAILURE_LINE_TOO_LONG, line_number, EFBIG);
    if (memchr(line, '\r', length))
        return input_fail(outcome, BX_FETCH_INPUT_FAILURE_INVALID_CONTROL, line_number, EINVAL);
    line[length] = '\0';
    *length_out = length;
    return 0;
}

int bx_fetch_input_urls_load_plain(const char* path, size_t reserved_entries, size_t reserved_bytes, BxFetchInputUrls* urls_out, BxFetchInputOutcome* outcome_out) {
    if (outcome_out)
        *outcome_out = (BxFetchInputOutcome){0};
    if (!path || path[0] == '\0' || !urls_out || !bx_fetch_resource_can_reserve(reserved_entries, reserved_bytes, 0, 0, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT, 0, EINVAL);
    }
    *urls_out = (BxFetchInputUrls){0};

    FILE* stream = input_open(path, outcome_out);
    if (!stream)
        return -1;

    char* line = malloc(BX_FETCH_URL_MAX_BYTES + 2u);
    if (!line) {
        int error_number = errno ? errno : ENOMEM;
        fclose(stream);
        return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_MEMORY, 1, error_number);
    }

    int result = 0;
    size_t line_number = 1;
    size_t total_bytes = 0;
    for (;;) {
        size_t length = 0;
        bool eof = false;
        if (input_read_line(stream, line, line_number, &total_bytes, &length, &eof, outcome_out) != 0) {
            result = -1;
            break;
        }
        if (eof)
            break;
        if (length > 0 && line[0] != '#' && input_urls_append(urls_out, line, length, reserved_entries, reserved_bytes, line_number, outcome_out) != 0) {
            result = -1;
            break;
        }
        line_number++;
    }

    free(line);
    int saved_error = errno;
    if (fclose(stream) != 0 && result == 0)
        result = input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_READ, line_number, errno);
    else if (result != 0)
        errno = saved_error;
    if (result != 0)
        bx_fetch_input_urls_free(urls_out);
    return result;
}

typedef struct {
    BxFetchInputUrls* urls;
    BxFetchInputOutcome* outcome;
    const char* base_url;
    size_t reserved_entries;
    size_t reserved_bytes;
    bool failed;
} HtmlInputContext;

static bool input_is_data_url(const char* value) {
    static const char prefix[] = "data:";
    return value && strncasecmp(value, prefix, sizeof(prefix) - 1u) == 0;
}

static void input_html_link(void* userdata, const char* reference) {
    HtmlInputContext* context = userdata;
    if (!context || context->failed || !reference || reference[0] == '\0')
        return;

    char* resolved = NULL;
    const char* value = reference;
    if (context->base_url) {
        resolved = bx_fetch_url_resolve(context->base_url, reference);
        if (!resolved)
            return;
        value = resolved;
    }
    if (input_is_data_url(value)) {
        free(resolved);
        return;
    }

    size_t length = 0;
    if (!bx_fetch_resource_bounded_strlen(value, BX_FETCH_URL_MAX_BYTES, &length)) {
        input_fail(context->outcome, BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT, 0, EFBIG);
        context->failed = true;
    }
    else if (input_urls_append(context->urls, value, length, context->reserved_entries, context->reserved_bytes, 0, context->outcome) != 0) {
        context->failed = true;
    }
    free(resolved);
}

static int input_read_html(FILE* stream, unsigned char** data_out, size_t* length_out, BxFetchInputOutcome* outcome) {
    unsigned char* data = NULL;
    size_t length = 0;
    size_t capacity = 0;
    unsigned char chunk[16 * 1024];

    for (;;) {
        size_t chunk_length = fread(chunk, 1, sizeof(chunk), stream);
        if (chunk_length > 0) {
            if (length > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES || chunk_length > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES - length) {
                free(data);
                return input_fail(outcome, BX_FETCH_INPUT_FAILURE_HTML_TOO_LARGE, 0, EFBIG);
            }
            size_t required = length + chunk_length;
            if (required > capacity) {
                size_t next_capacity = capacity ? capacity : sizeof(chunk);
                while (next_capacity < required)
                    next_capacity = next_capacity > BX_FETCH_DOCUMENT_PARSE_MAX_BYTES / 2u ? required : next_capacity * 2u;
                unsigned char* grown = realloc(data, next_capacity);
                if (!grown) {
                    free(data);
                    return input_fail(outcome, BX_FETCH_INPUT_FAILURE_MEMORY, 0, ENOMEM);
                }
                data = grown;
                capacity = next_capacity;
            }
            memcpy(data + length, chunk, chunk_length);
            length += chunk_length;
        }
        if (chunk_length < sizeof(chunk)) {
            if (ferror(stream)) {
                int error_number = errno ? errno : EIO;
                free(data);
                return input_fail(outcome, BX_FETCH_INPUT_FAILURE_READ, 0, error_number);
            }
            if (feof(stream))
                break;
        }
    }
    if (!data) {
        data = malloc(1);
        if (!data)
            return input_fail(outcome, BX_FETCH_INPUT_FAILURE_MEMORY, 0, ENOMEM);
    }
    *data_out = data;
    *length_out = length;
    return 0;
}

int bx_fetch_input_urls_load_html(const char* path, const char* base_url, size_t reserved_entries, size_t reserved_bytes, BxFetchInputUrls* urls_out, BxFetchInputOutcome* outcome_out) {
    if (outcome_out)
        *outcome_out = (BxFetchInputOutcome){0};
    if (!path || path[0] == '\0' || !urls_out || !bx_fetch_resource_can_reserve(reserved_entries, reserved_bytes, 0, 0, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES))
        return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_URL_STATE_LIMIT, 0, EINVAL);
    *urls_out = (BxFetchInputUrls){0};

    BxFetchPreparedUrl* prepared_base = NULL;
    if (base_url) {
        prepared_base = bx_fetch_url_prepare(base_url);
        if (!prepared_base)
            return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_BASE_URL, 0, errno);
    }

    FILE* stream = input_open(path, outcome_out);
    if (!stream) {
        bx_fetch_prepared_url_free(prepared_base);
        return -1;
    }
    unsigned char* data = NULL;
    size_t length = 0;
    int result = input_read_html(stream, &data, &length, outcome_out);
    int saved_error = errno;
    if (fclose(stream) != 0 && result == 0)
        result = input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_READ, 0, errno);
    else if (result != 0)
        errno = saved_error;
    if (result != 0)
        goto cleanup;

    HtmlInputContext context = {
        .urls = urls_out,
        .outcome = outcome_out,
        .base_url = prepared_base ? bx_fetch_prepared_url_transport(prepared_base) : NULL,
        .reserved_entries = reserved_entries,
        .reserved_bytes = reserved_bytes,
    };
    errno = 0;
    if (bx_fetch_html_extract_links(NULL, (const char*)data, length, input_html_link, &context) != 0 && !context.failed)
        result = input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_HTML_PARSE, 0, errno ? errno : EINVAL);
    else if (context.failed)
        result = -1;

cleanup:
    free(data);
    bx_fetch_prepared_url_free(prepared_base);
    if (result != 0)
        bx_fetch_input_urls_free(urls_out);
    return result;
}
