#ifndef BX_APPLETS_SHELL_ASH_PARSER_H
#define BX_APPLETS_SHELL_ASH_PARSER_H

#include <stddef.h>

#include "applets/shell/ash/ast.h"
#include "applets/shell/ash/lexer.h"

enum ash_parser_result {
    ASH_PARSER_COMPLETE = 0,
    ASH_PARSER_INCOMPLETE,
    ASH_PARSER_ERROR,
};

struct ash_parser {
    struct ash_lexer lexer;
    struct ash_token lookahead;
    bool has_lookahead;
    enum ash_parser_result result;
    struct ash_source_location error_location;
    const char* error;
};

void ash_parser_init(
    struct ash_parser* parser,
    const char* source_name,
    const char* input,
    size_t length
);
void ash_parser_destroy(struct ash_parser* parser);
enum ash_parser_result ash_parser_parse_program(
    struct ash_parser* parser,
    struct ash_ast** program
);

#endif /* BX_APPLETS_SHELL_ASH_PARSER_H */
