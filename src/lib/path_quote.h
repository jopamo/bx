#ifndef BX_COMMON_PATH_QUOTE_H
#define BX_COMMON_PATH_QUOTE_H

#include <stdbool.h>

enum bx_path_quote_style {
    BX_PATH_QUOTE_LITERAL = 0,
    BX_PATH_QUOTE_LITERAL_HIDE_NONGRAPHIC,
    BX_PATH_QUOTE_ESCAPE,
    BX_PATH_QUOTE_C,
    BX_PATH_QUOTE_LOCALE,
    BX_PATH_QUOTE_SHELL,
    BX_PATH_QUOTE_SHELL_ALWAYS,
    BX_PATH_QUOTE_SHELL_ESCAPE,
    BX_PATH_QUOTE_SHELL_ESCAPE_ALWAYS,
    BX_PATH_QUOTE_SINGLE_BACKSLASH,
};

enum bx_path_control_quote_style {
    BX_PATH_CONTROL_QUOTE_LITERAL = 0,
    BX_PATH_CONTROL_QUOTE_QUESTION,
    BX_PATH_CONTROL_QUOTE_CARET,
};

struct bx_path_control_quote_options {
    enum bx_path_control_quote_style style;
    bool high_bit_printable;
};

char* bx_path_quote_dup(const char* text, enum bx_path_quote_style style);
char* bx_path_quote_control_dup(const char* text, const struct bx_path_control_quote_options* options);

#endif /* BX_COMMON_PATH_QUOTE_H */
