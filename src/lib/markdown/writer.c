#include "lib/markdown/writer.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool writer_reserve(BxMarkdownWriter* writer, size_t additional) {
    if (!writer || writer->length > writer->limit || additional > writer->limit - writer->length) {
        errno = EFBIG;
        return false;
    }
    if (writer->length + additional == SIZE_MAX) {
        errno = EFBIG;
        return false;
    }
    size_t needed = writer->length + additional + 1u;
    if (needed <= writer->capacity)
        return true;

    size_t capacity = writer->capacity ? writer->capacity : 1024u;
    while (capacity < needed) {
        size_t next = capacity <= (SIZE_MAX / 2u) ? capacity * 2u : SIZE_MAX;
        size_t maximum = writer->limit == SIZE_MAX ? SIZE_MAX : writer->limit + 1u;
        if (next <= capacity || next > maximum) {
            capacity = maximum;
            break;
        }
        capacity = next;
    }
    if (capacity < needed) {
        errno = EFBIG;
        return false;
    }
    char* grown = realloc(writer->data, capacity);
    if (!grown)
        return false;
    writer->data = grown;
    writer->capacity = capacity;
    return true;
}

static bool writer_append(BxMarkdownWriter* writer, const char* data, size_t length) {
    if (length == 0)
        return true;
    if (!data || !writer_reserve(writer, length)) {
        if (!data)
            errno = EINVAL;
        return false;
    }
    memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    writer->data[writer->length] = '\0';
    return true;
}

static bool writer_flush_space(BxMarkdownWriter* writer) {
    if (!writer || !writer->pending_space)
        return writer != NULL;
    writer->pending_space = false;
    if (writer->length == 0 || writer->data[writer->length - 1u] == '\n' || writer->data[writer->length - 1u] == ' ')
        return true;
    return writer_append(writer, " ", 1u);
}

void bx_markdown_writer_init(BxMarkdownWriter* writer, size_t limit) {
    if (writer)
        *writer = (BxMarkdownWriter){.limit = limit};
}

void bx_markdown_writer_clear(BxMarkdownWriter* writer) {
    if (!writer)
        return;
    free(writer->data);
    *writer = (BxMarkdownWriter){0};
}

bool bx_markdown_writer_raw(BxMarkdownWriter* writer, const char* data, size_t length) {
    return writer_flush_space(writer) && writer_append(writer, data, length);
}

static bool markdown_text_character_needs_escape(unsigned char c) {
    switch (c) {
        case '\\':
        case '`':
        case '*':
        case '_':
        case '[':
        case ']':
        case '<':
        case '>':
        case '#':
        case '|':
            return true;
        default:
            return false;
    }
}

bool bx_markdown_writer_text(BxMarkdownWriter* writer, const char* data, size_t length) {
    if (!writer || (!data && length > 0)) {
        errno = EINVAL;
        return false;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char c = (unsigned char)data[index];
        if (isspace(c)) {
            if (writer->length > 0)
                writer->pending_space = true;
            continue;
        }
        if (!writer_flush_space(writer))
            return false;
        if (markdown_text_character_needs_escape(c) && !writer_append(writer, "\\", 1u))
            return false;
        if (!writer_append(writer, (const char*)&data[index], 1u))
            return false;
    }
    return true;
}

bool bx_markdown_writer_newlines(BxMarkdownWriter* writer, size_t count) {
    if (!writer) {
        errno = EINVAL;
        return false;
    }
    writer->pending_space = false;
    if (writer->length == 0)
        return true;
    while (writer->length > 0 && (writer->data[writer->length - 1u] == ' ' || writer->data[writer->length - 1u] == '\t'))
        writer->length--;
    size_t existing = 0;
    while (existing < writer->length && writer->data[writer->length - existing - 1u] == '\n')
        existing++;
    if (existing >= count)
        return true;
    static const char newlines[] = "\n\n";
    while (existing < count) {
        size_t chunk = count - existing;
        if (chunk > sizeof(newlines) - 1u)
            chunk = sizeof(newlines) - 1u;
        if (!writer_append(writer, newlines, chunk))
            return false;
        existing += chunk;
    }
    return true;
}

static size_t longest_run(const char* data, size_t length, char needle) {
    size_t longest = 0;
    size_t current = 0;
    for (size_t index = 0; index < length; index++) {
        if (data[index] == needle) {
            current++;
            if (current > longest)
                longest = current;
        }
        else {
            current = 0;
        }
    }
    return longest;
}

static bool writer_repeat(BxMarkdownWriter* writer, char c, size_t count) {
    char repeated[32];
    memset(repeated, c, sizeof(repeated));
    while (count > 0) {
        size_t chunk = count < sizeof(repeated) ? count : sizeof(repeated);
        if (!writer_append(writer, repeated, chunk))
            return false;
        count -= chunk;
    }
    return true;
}

bool bx_markdown_writer_code_span(BxMarkdownWriter* writer, const char* data, size_t length) {
    if (!writer || (!data && length > 0)) {
        errno = EINVAL;
        return false;
    }
    size_t delimiter = longest_run(data, length, '`') + 1u;
    bool pad = length > 0 && (data[0] == '`' || data[length - 1u] == '`' || data[0] == ' ' || data[length - 1u] == ' ');
    if (!writer_flush_space(writer) || !writer_repeat(writer, '`', delimiter) || (pad && !writer_append(writer, " ", 1u)))
        return false;
    for (size_t index = 0; index < length; index++) {
        char c = data[index];
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
        if (!writer_append(writer, &c, 1u))
            return false;
    }
    return (!pad || writer_append(writer, " ", 1u)) && writer_repeat(writer, '`', delimiter);
}

bool bx_markdown_writer_code_block(BxMarkdownWriter* writer, const char* info, const char* data, size_t length) {
    if (!writer || (!data && length > 0)) {
        errno = EINVAL;
        return false;
    }
    size_t fence = longest_run(data, length, '`') + 1u;
    if (fence < 3u)
        fence = 3u;
    if (!bx_markdown_writer_newlines(writer, 2u) || !writer_repeat(writer, '`', fence))
        return false;
    if (info && info[0] != '\0' && !writer_append(writer, info, strlen(info)))
        return false;
    if (!writer_append(writer, "\n", 1u) || !writer_append(writer, data, length))
        return false;
    if (length > 0 && data[length - 1u] != '\n' && !writer_append(writer, "\n", 1u))
        return false;
    return writer_repeat(writer, '`', fence) && bx_markdown_writer_newlines(writer, 2u);
}

char* bx_markdown_writer_take(BxMarkdownWriter* writer, size_t* length_out) {
    if (!writer) {
        errno = EINVAL;
        return NULL;
    }
    writer->pending_space = false;
    while (writer->length > 0 && isspace((unsigned char)writer->data[writer->length - 1u]))
        writer->length--;
    if (writer->length > 0 && !writer_append(writer, "\n", 1u))
        return NULL;
    if (!writer->data) {
        writer->data = strdup("");
        if (!writer->data)
            return NULL;
        writer->capacity = 1u;
    }
    char* result = writer->data;
    if (length_out)
        *length_out = writer->length;
    writer->data = NULL;
    writer->length = 0;
    writer->capacity = 0;
    return result;
}
