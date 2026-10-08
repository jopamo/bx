#include <string.h>

#include "applets/shell/ash/here_document.h"
#include "lib/text_buffer.h"

enum ash_here_document_line_result
ash_here_document_match_line(struct bx_text_buffer* logical_line, const char* physical_line, size_t physical_length, const char* delimiter, size_t delimiter_length, bool quoted, bool strip_tabs) {
    bool has_newline = physical_length != 0u && physical_line[physical_length - 1u] == '\n';
    size_t end = physical_length - (has_newline ? 1u : 0u);
    size_t start = 0u;
    if (logical_line->length == 0u && strip_tabs) {
        while (start < end && physical_line[start] == '\t') {
            start++;
        }
    }
    if (!bx_text_buffer_append_span(logical_line, physical_line + start, end - start)) {
        return ASH_HERE_DOCUMENT_LINE_ERROR;
    }
    size_t backslashes = 0u;
    for (size_t i = end; i > start && physical_line[i - 1u] == '\\'; i--) {
        backslashes++;
    }
    if (!quoted && has_newline && backslashes % 2u != 0u) {
        logical_line->data[--logical_line->length] = '\0';
        return ASH_HERE_DOCUMENT_LINE_CONTINUED;
    }
    bool matched = logical_line->length == delimiter_length && (delimiter_length == 0u || memcmp(logical_line->data, delimiter, delimiter_length) == 0);
    bx_text_buffer_clear(logical_line);
    return matched ? ASH_HERE_DOCUMENT_LINE_DELIMITER : ASH_HERE_DOCUMENT_LINE_BODY;
}
