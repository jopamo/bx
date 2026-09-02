#ifndef BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H
#define BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H

#include "applets/shell/ash/parser.h"

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

#endif /* BX_APPLETS_SHELL_ASH_PARSER_INTERNAL_H */
