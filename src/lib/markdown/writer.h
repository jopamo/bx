#ifndef BX_MARKDOWN_WRITER_H
#define BX_MARKDOWN_WRITER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
    size_t limit;
    bool pending_space;
} BxMarkdownWriter;

void bx_markdown_writer_init(BxMarkdownWriter* writer, size_t limit);
void bx_markdown_writer_clear(BxMarkdownWriter* writer);
bool bx_markdown_writer_raw(BxMarkdownWriter* writer, const char* data, size_t length);
bool bx_markdown_writer_text(BxMarkdownWriter* writer, const char* data, size_t length);
bool bx_markdown_writer_newlines(BxMarkdownWriter* writer, size_t count);
bool bx_markdown_writer_code_span(BxMarkdownWriter* writer, const char* data, size_t length);
bool bx_markdown_writer_code_block(BxMarkdownWriter* writer, const char* info, const char* data, size_t length);
char* bx_markdown_writer_take(BxMarkdownWriter* writer, size_t* length_out);

#endif
