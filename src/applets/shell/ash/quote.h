#ifndef BX_APPLETS_SHELL_ASH_QUOTE_H
#define BX_APPLETS_SHELL_ASH_QUOTE_H

#include <stdbool.h>
#include <stddef.h>

struct bx_text_buffer;

bool ash_quote_append_dollar_single(struct bx_text_buffer* output, const char* text, size_t length);

#endif /* BX_APPLETS_SHELL_ASH_QUOTE_H */
