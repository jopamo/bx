#ifndef BX_COMMON_OUTPUT_QUOTE_H
#define BX_COMMON_OUTPUT_QUOTE_H

#include <stdbool.h>

enum bx_output_quote_style {
    BX_OUTPUT_QUOTE_LITERAL = 0,
    BX_OUTPUT_QUOTE_LITERAL_HIDE_NONGRAPHIC,
    BX_OUTPUT_QUOTE_ESCAPE,
    BX_OUTPUT_QUOTE_C,
    BX_OUTPUT_QUOTE_LOCALE,
    BX_OUTPUT_QUOTE_SHELL,
    BX_OUTPUT_QUOTE_SHELL_ALWAYS,
    BX_OUTPUT_QUOTE_SHELL_ESCAPE,
    BX_OUTPUT_QUOTE_SHELL_ESCAPE_ALWAYS,
    BX_OUTPUT_QUOTE_SHELL_REUSABLE,
    BX_OUTPUT_QUOTE_SINGLE_BACKSLASH,
};

enum bx_output_control_quote_style {
    BX_OUTPUT_CONTROL_QUOTE_LITERAL = 0,
    BX_OUTPUT_CONTROL_QUOTE_QUESTION,
    BX_OUTPUT_CONTROL_QUOTE_CARET,
};

struct bx_output_control_quote_options {
    enum bx_output_control_quote_style style;
    bool high_bit_printable;
};

char* bx_output_quote_dup(const char* text, enum bx_output_quote_style style);
char* bx_output_quote_control_dup(const char* text, const struct bx_output_control_quote_options* options);
char* bx_output_quote_shell_reusable_dup(const char* text);
bool bx_output_quote_terminal_should_hide_control(int fd);

#endif /* BX_COMMON_OUTPUT_QUOTE_H */
