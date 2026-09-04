#define _GNU_SOURCE
#include "lib/fetch/input.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fd_ops.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    int fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0)
        return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_OPEN, 0, errno);
    FILE* stream = fdopen(fd, "r");
    if (!stream) {
        int error_number = errno;
        bx_fd_cleanup(&fd);
        return input_fail(outcome_out, BX_FETCH_INPUT_FAILURE_OPEN, 0, error_number);
    }

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
