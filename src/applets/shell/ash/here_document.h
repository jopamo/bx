#ifndef BX_APPLETS_SHELL_ASH_HERE_DOCUMENT_H
#define BX_APPLETS_SHELL_ASH_HERE_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

struct bx_text_buffer;

struct ash_here_document_read_state {
    bool line_start;
    bool escaped;
};

void ash_here_document_read_advance(struct ash_here_document_read_state* state, char byte, bool quoted);

/* Normalize owned bytes in place; retain a trailing NUL outside the length. */
size_t ash_here_document_normalize_span(char* body, size_t length, bool quoted, bool strip_tabs, bool line_start);
bool ash_here_document_needs_normalization(const char* body, size_t length, bool quoted, bool strip_tabs, bool line_start);

enum ash_here_document_line_result {
    ASH_HERE_DOCUMENT_LINE_ERROR,
    ASH_HERE_DOCUMENT_LINE_CONTINUED,
    ASH_HERE_DOCUMENT_LINE_BODY,
    ASH_HERE_DOCUMENT_LINE_DELIMITER,
};

/* Retain logical_line across continuations; clear it after a complete line. */
enum ash_here_document_line_result
ash_here_document_match_line(struct bx_text_buffer* logical_line, const char* physical_line, size_t physical_length, const char* delimiter, size_t delimiter_length, bool quoted, bool strip_tabs);

#endif /* BX_APPLETS_SHELL_ASH_HERE_DOCUMENT_H */
