#ifndef BX_APPLETS_SHELL_ASH_LEXER_H
#define BX_APPLETS_SHELL_ASH_LEXER_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/shell/ash/syntax.h"

enum ash_token_kind {
    ASH_TOKEN_EOF = 0,
    ASH_TOKEN_WORD,
    ASH_TOKEN_IO_NUMBER,
    ASH_TOKEN_IO_VARIABLE,
    ASH_TOKEN_NEWLINE,
    ASH_TOKEN_AND_IF,
    ASH_TOKEN_OR_IF,
    ASH_TOKEN_DSEMI,
    ASH_TOKEN_SEMI_AND,
    ASH_TOKEN_DSEMI_AND,
    ASH_TOKEN_PIPE,
    ASH_TOKEN_PIPE_AND,
    ASH_TOKEN_AMP,
    ASH_TOKEN_SEMI,
    ASH_TOKEN_LPAREN,
    ASH_TOKEN_RPAREN,
    ASH_TOKEN_LESS,
    ASH_TOKEN_GREAT,
    ASH_TOKEN_DLESS,
    ASH_TOKEN_DLESS_DASH,
    ASH_TOKEN_TLESS,
    ASH_TOKEN_DGREAT,
    ASH_TOKEN_LESS_AND,
    ASH_TOKEN_GREAT_AND,
    ASH_TOKEN_LESS_GREAT,
    ASH_TOKEN_CLOBBER,
    ASH_TOKEN_AND_GREAT,
    ASH_TOKEN_AND_DGREAT,
    ASH_TOKEN_COUNT,
};

struct ash_token {
    enum ash_token_kind kind;
    struct ash_source_location location;
    struct ash_word word;
    /* Owned number text or variable name for an IO-prefix token. */
    char* io_redirect;
};

enum ash_lexer_result {
    ASH_LEXER_TOKEN = 0,
    ASH_LEXER_END,
    ASH_LEXER_INCOMPLETE,
    ASH_LEXER_ERROR,
};

struct ash_lexer {
    const char* source_name;
    struct ash_source_identity* source_identity;
    const char* input;
    size_t length;
    size_t offset;
    size_t source_offset;
    size_t line;
    size_t column;
    bool ended_with_line_continuation;
    struct ash_source_location error_location;
    const char* error;
};

void ash_lexer_init(
    struct ash_lexer* lexer,
    const char* source_name,
    const char* input,
    size_t length
);
void ash_lexer_init_at(
    struct ash_lexer* lexer,
    struct ash_source_location origin,
    const char* input,
    size_t length
);
struct ash_source_location ash_lexer_current_location(
    const struct ash_lexer* lexer
);
bool ash_lexer_ended_with_line_continuation(
    const struct ash_lexer* lexer
);
enum ash_lexer_result ash_lexer_next(struct ash_lexer* lexer, struct ash_token* token);
void ash_token_destroy(struct ash_token* token);
const char* ash_token_kind_name(enum ash_token_kind kind);
bool ash_token_kind_valid(enum ash_token_kind kind);
bool ash_token_is_redirection_prefix(enum ash_token_kind kind);
bool ash_token_is_redirection(enum ash_token_kind kind);

#endif /* BX_APPLETS_SHELL_ASH_LEXER_H */
