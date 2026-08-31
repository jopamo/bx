#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lib/text_buffer.h"

void bx_text_buffer_init(struct bx_text_buffer* buffer) {
    *buffer = (struct bx_text_buffer){0};
}

void bx_text_buffer_clear(struct bx_text_buffer* buffer) {
    buffer->length = 0u;
    if (buffer->data != NULL) {
        buffer->data[0] = '\0';
    }
}

void bx_text_buffer_destroy(struct bx_text_buffer* buffer) {
    free(buffer->data);
    *buffer = (struct bx_text_buffer){0};
}

static bool bx_text_buffer_reserve(
    struct bx_text_buffer* buffer,
    size_t needed
) {
    if (buffer->capacity >= needed) {
        return true;
    }

    size_t capacity = buffer->capacity == 0u ? 32u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }

    char* replacement = realloc(buffer->data, capacity);
    if (replacement == NULL) {
        errno = ENOMEM;
        return false;
    }
    buffer->data = replacement;
    buffer->capacity = capacity;
    return true;
}

bool bx_text_buffer_append_char(
    struct bx_text_buffer* buffer,
    char character
) {
    if (buffer->length > SIZE_MAX - 2u ||
        !bx_text_buffer_reserve(buffer, buffer->length + 2u)) {
        errno = ENOMEM;
        return false;
    }
    buffer->data[buffer->length++] = character;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool bx_text_buffer_append_span(
    struct bx_text_buffer* buffer,
    const char* text,
    size_t length
) {
    if (length > SIZE_MAX - buffer->length - 1u ||
        !bx_text_buffer_reserve(buffer, buffer->length + length + 1u)) {
        errno = ENOMEM;
        return false;
    }
    if (length != 0u) {
        memcpy(buffer->data + buffer->length, text, length);
    }
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

bool bx_text_buffer_append_text(
    struct bx_text_buffer* buffer,
    const char* text
) {
    return bx_text_buffer_append_span(buffer, text, strlen(text));
}

char* bx_text_buffer_take(struct bx_text_buffer* buffer) {
    if (buffer->data == NULL) {
        char* empty = malloc(1u);
        if (empty == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        empty[0] = '\0';
        return empty;
    }

    char* result = buffer->data;
    *buffer = (struct bx_text_buffer){0};
    return result;
}
