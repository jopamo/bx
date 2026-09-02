#ifndef BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H
#define BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H

#include "applets/shell/ash/parser.h"

struct bx_text_buffer;

typedef bool (*ash_parser_reserved_word_fn)(
    const struct ash_token* token
);

enum ash_parser_result ash_parser_fail(
    struct ash_parser* parser,
    enum ash_parser_result result,
    struct ash_source_location location,
    const char* error
);
struct ash_token* ash_parser_peek(struct ash_parser* parser);
bool ash_parser_take(
    struct ash_parser* parser,
    struct ash_token* token
);
enum ash_parser_raw_line_result {
    ASH_PARSER_RAW_LINE = 0,
    ASH_PARSER_RAW_END,
    ASH_PARSER_RAW_ERROR,
};
enum ash_parser_raw_line_result ash_parser_take_raw_line(
    struct ash_parser* parser,
    struct bx_text_buffer* line,
    struct ash_source_location* location
);
bool ash_parser_prepare_alias(
    struct ash_parser* parser,
    bool command_position,
    bool reserved_word_precedes_alias,
    ash_parser_reserved_word_fn is_reserved_word
);
bool ash_parser_prepare_command_alias(
    struct ash_parser* parser,
    ash_parser_reserved_word_fn is_reserved_word
);
void ash_parser_alias_state_init(
    struct ash_parser* parser,
    const struct ash_alias_table* aliases
);
void ash_parser_alias_state_destroy(struct ash_parser* parser);
bool ash_parser_register_here_document(
    struct ash_parser* parser,
    struct ash_redirection* redirection
);
bool ash_parser_consume_here_documents(struct ash_parser* parser);
bool ash_parser_has_pending_here_documents(
    const struct ash_parser* parser
);
void ash_parser_discard_pending_here_documents(
    struct ash_parser* parser
);
void ash_parser_here_document_state_destroy(
    struct ash_parser* parser
);

#endif /* BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H */
