#ifndef BX_APPLETS_SHELL_ASH_PARSER_H
#define BX_APPLETS_SHELL_ASH_PARSER_H

#include <stddef.h>

#include "applets/shell/ash/ast.h"
#include "applets/shell/ash/lexer.h"

struct ash_alias;
struct ash_alias_table;

enum ash_parser_result {
    ASH_PARSER_COMPLETE = 0,
    ASH_PARSER_INCOMPLETE,
    ASH_PARSER_ERROR,
};

#define ASH_PARSER_INLINE_ALIAS_FRAMES 4u

struct ash_parser_alias_release {
    const struct ash_alias* alias;
    size_t offset;
};

struct ash_parser_alias_frame {
    struct ash_lexer lexer;
    const struct ash_alias* alias;
    bool continue_alias;
    bool alias_active;
    size_t alias_length;
    char* owned_input;
    struct ash_parser_alias_release* releases;
    size_t release_count;
};

struct ash_here_document;

struct ash_parser_config {
    const struct ash_alias_table* aliases;
    struct ash_lexer_options lexer;
};

struct ash_parser {
    struct ash_lexer lexer;
    struct ash_token lookahead;
    bool has_lookahead;
    bool lookahead_alias_checked;
    enum ash_parser_result result;
    struct ash_source_location error_location;
    const char* error;
    const struct ash_alias_table* aliases;
    struct ash_parser_alias_frame* alias_frames;
    size_t alias_frame_count;
    size_t alias_frame_capacity;
    struct ash_parser_alias_frame
        inline_alias_frames[ASH_PARSER_INLINE_ALIAS_FRAMES];
    bool continue_alias;
    bool alias_comment_elided;
    /*
     * Borrowed AST-owned documents awaiting the next newline boundary.
     * Completed documents leave this queue before an AST is published.
     */
    struct ash_here_document** pending_here_documents;
    size_t pending_here_document_count;
    size_t pending_here_document_capacity;
};

void ash_parser_init(
    struct ash_parser* parser,
    const char* source_name,
    const char* input,
    size_t length
);
void ash_parser_init_at(
    struct ash_parser* parser,
    struct ash_source_location origin,
    const char* input,
    size_t length
);
void ash_parser_init_at_with_config(
    struct ash_parser* parser,
    struct ash_source_location origin,
    const char* input,
    size_t length,
    const struct ash_parser_config* config
);
void ash_parser_destroy(struct ash_parser* parser);
enum ash_parser_result ash_parser_parse_program(
    struct ash_parser* parser,
    struct ash_ast** program
);

#endif /* BX_APPLETS_SHELL_ASH_PARSER_H */
