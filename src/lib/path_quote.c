#include <stddef.h>

#include "lib/output_quote.h"
#include "lib/path_quote.h"

static enum bx_output_quote_style bx_path_quote_output_style(enum bx_path_quote_style style) {
    switch (style) {
        case BX_PATH_QUOTE_LITERAL:
            return BX_OUTPUT_QUOTE_LITERAL;
        case BX_PATH_QUOTE_LITERAL_HIDE_NONGRAPHIC:
            return BX_OUTPUT_QUOTE_LITERAL_HIDE_NONGRAPHIC;
        case BX_PATH_QUOTE_ESCAPE:
            return BX_OUTPUT_QUOTE_ESCAPE;
        case BX_PATH_QUOTE_C:
            return BX_OUTPUT_QUOTE_C;
        case BX_PATH_QUOTE_LOCALE:
            return BX_OUTPUT_QUOTE_LOCALE;
        case BX_PATH_QUOTE_SHELL:
            return BX_OUTPUT_QUOTE_SHELL;
        case BX_PATH_QUOTE_SHELL_ALWAYS:
            return BX_OUTPUT_QUOTE_SHELL_ALWAYS;
        case BX_PATH_QUOTE_SHELL_ESCAPE:
            return BX_OUTPUT_QUOTE_SHELL_ESCAPE;
        case BX_PATH_QUOTE_SHELL_ESCAPE_ALWAYS:
            return BX_OUTPUT_QUOTE_SHELL_ESCAPE_ALWAYS;
        case BX_PATH_QUOTE_SINGLE_BACKSLASH:
            return BX_OUTPUT_QUOTE_SINGLE_BACKSLASH;
        default:
            return BX_OUTPUT_QUOTE_LITERAL;
    }
}

static enum bx_output_control_quote_style bx_path_quote_output_control_style(enum bx_path_control_quote_style style) {
    switch (style) {
        case BX_PATH_CONTROL_QUOTE_LITERAL:
            return BX_OUTPUT_CONTROL_QUOTE_LITERAL;
        case BX_PATH_CONTROL_QUOTE_QUESTION:
            return BX_OUTPUT_CONTROL_QUOTE_QUESTION;
        case BX_PATH_CONTROL_QUOTE_CARET:
            return BX_OUTPUT_CONTROL_QUOTE_CARET;
        default:
            return BX_OUTPUT_CONTROL_QUOTE_LITERAL;
    }
}

char* bx_path_quote_dup(const char* text, enum bx_path_quote_style style) {
    return bx_output_quote_dup(text, bx_path_quote_output_style(style));
}

char* bx_path_quote_control_dup(const char* text, const struct bx_path_control_quote_options* options) {
    struct bx_output_control_quote_options output_options = {
        .style = BX_OUTPUT_CONTROL_QUOTE_LITERAL,
        .high_bit_printable = false,
    };

    if (options != NULL) {
        output_options.style = bx_path_quote_output_control_style(options->style);
        output_options.high_bit_printable = options->high_bit_printable;
    }

    return bx_output_quote_control_dup(text, &output_options);
}
