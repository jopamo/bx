#include <string.h>

#include "applets/shell/ash/here_document.h"
#include "lib/text_buffer.h"

void ash_here_document_read_advance(struct ash_here_document_read_state* state, char byte, bool quoted) {
    if (!quoted && state->escaped) {
        if (byte != '\n') {
            state->line_start = false;
        }
        state->escaped = false;
    }
    else if (!quoted && byte == '\\') {
        state->escaped = true;
    }
    else if (byte != '\t' || !state->line_start) {
        state->line_start = byte == '\n';
    }
}

static size_t ash_here_document_normalize(const char* body, size_t length, bool quoted, bool strip_tabs, bool line_start, char* normalized) {
    size_t output = 0u;
    struct ash_here_document_read_state state = {.line_start = line_start};
    for (size_t input = 0u; input < length;) {
        char byte = body[input++];
        bool leading_tab = state.line_start && !state.escaped && byte == '\t';
        ash_here_document_read_advance(&state, byte, quoted);
        if (!quoted && byte == '\\' && input < length) {
            char next = body[input++];
            ash_here_document_read_advance(&state, next, quoted);
            if (next == '\n') {
                continue;
            }
            if (normalized != NULL) {
                normalized[output] = byte;
                normalized[output + 1u] = next;
            }
            output += 2u;
            continue;
        }
        if (strip_tabs && leading_tab) {
            continue;
        }
        if (normalized != NULL) {
            normalized[output] = byte;
        }
        output++;
    }
    if (length != 0u && normalized != NULL) {
        normalized[output] = '\0';
    }
    return output;
}

size_t ash_here_document_normalize_span(char* body, size_t length, bool quoted, bool strip_tabs, bool line_start) {
    return ash_here_document_normalize(body, length, quoted, strip_tabs, line_start, body);
}

bool ash_here_document_needs_normalization(const char* body, size_t length, bool quoted, bool strip_tabs, bool line_start) {
    return ash_here_document_normalize(body, length, quoted, strip_tabs, line_start, NULL) != length;
}

enum ash_here_document_line_result
ash_here_document_match_line(struct bx_text_buffer* logical_line, const char* physical_line, size_t physical_length, const char* delimiter, size_t delimiter_length, bool quoted, bool strip_tabs) {
    bool has_newline = physical_length != 0u && physical_line[physical_length - 1u] == '\n';
    size_t end = physical_length - (has_newline ? 1u : 0u);
    size_t previous_length = logical_line->length;
    if (!bx_text_buffer_append_span(logical_line, physical_line, physical_length)) {
        return ASH_HERE_DOCUMENT_LINE_ERROR;
    }
    logical_line->length = previous_length + ash_here_document_normalize_span(logical_line->data + previous_length, physical_length, quoted, false, true);
    size_t backslashes = 0u;
    for (size_t i = end; i > 0u && physical_line[i - 1u] == '\\'; i--) {
        backslashes++;
    }
    if (!quoted && has_newline && backslashes % 2u != 0u) {
        return ASH_HERE_DOCUMENT_LINE_CONTINUED;
    }
    if (has_newline) {
        logical_line->data[--logical_line->length] = '\0';
    }
    logical_line->length = ash_here_document_normalize_span(logical_line->data, logical_line->length, true, strip_tabs, true);
    bool matched = logical_line->length == delimiter_length && (delimiter_length == 0u || memcmp(logical_line->data, delimiter, delimiter_length) == 0);
    bx_text_buffer_clear(logical_line);
    return matched ? ASH_HERE_DOCUMENT_LINE_DELIMITER : ASH_HERE_DOCUMENT_LINE_BODY;
}
