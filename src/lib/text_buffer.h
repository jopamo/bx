#ifndef BX_LIB_TEXT_BUFFER_H
#define BX_LIB_TEXT_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

struct bx_text_buffer {
    char* data;
    size_t length;
    size_t capacity;
};

void bx_text_buffer_init(struct bx_text_buffer* buffer);
void bx_text_buffer_clear(struct bx_text_buffer* buffer);
void bx_text_buffer_destroy(struct bx_text_buffer* buffer);
/* Content and length remain unchanged if reserve fails. */
bool bx_text_buffer_reserve(
    struct bx_text_buffer* buffer,
    size_t needed
);
bool bx_text_buffer_append_char(struct bx_text_buffer* buffer, char character);
bool bx_text_buffer_append_span(
    struct bx_text_buffer* buffer,
    const char* text,
    size_t length
);
bool bx_text_buffer_append_text(
    struct bx_text_buffer* buffer,
    const char* text
);
char* bx_text_buffer_take(struct bx_text_buffer* buffer);

#endif /* BX_LIB_TEXT_BUFFER_H */
