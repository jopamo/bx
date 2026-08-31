#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/parser.h"

struct ash_parse_stop {
    enum ash_token_kind token;
    const char* words[4];
    bool require_separator;
};

static enum ash_parser_result ash_parser_fail(
    struct ash_parser* parser,
    enum ash_parser_result result,
    struct ash_source_location location,
    const char* error
) {
    if (parser->result == ASH_PARSER_COMPLETE) {
        parser->result = result;
        parser->error_location = location;
        parser->error = error;
    }
    return parser->result;
}

static enum ash_parser_result ash_parser_fill(struct ash_parser* parser) {
    if (parser->result != ASH_PARSER_COMPLETE) {
        return parser->result;
    }
    if (parser->has_lookahead) {
        return ASH_PARSER_COMPLETE;
    }

    enum ash_lexer_result result = ash_lexer_next(&parser->lexer, &parser->lookahead);
    if (result == ASH_LEXER_INCOMPLETE) {
        return ash_parser_fail(
            parser,
            ASH_PARSER_INCOMPLETE,
            parser->lexer.error_location,
            parser->lexer.error
        );
    }
    if (result == ASH_LEXER_ERROR) {
        return ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            parser->lexer.error_location,
            parser->lexer.error
        );
    }
    parser->has_lookahead = true;
    return ASH_PARSER_COMPLETE;
}

static struct ash_token* ash_parser_peek(struct ash_parser* parser) {
    if (ash_parser_fill(parser) != ASH_PARSER_COMPLETE) {
        return NULL;
    }
    return &parser->lookahead;
}

static bool ash_parser_take(struct ash_parser* parser, struct ash_token* token) {
    if (ash_parser_fill(parser) != ASH_PARSER_COMPLETE) {
        return false;
    }
    *token = parser->lookahead;
    parser->lookahead = (struct ash_token){0};
    parser->has_lookahead = false;
    return true;
}

static bool ash_parser_at_stop(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }
    if (token->kind == ASH_TOKEN_WORD) {
        for (size_t i = 0u; i < sizeof(stop->words) / sizeof(stop->words[0]); i++) {
            if (stop->words[i] != NULL &&
                ash_word_is_unquoted_literal(&token->word, stop->words[i])) {
                return true;
            }
        }
    }
    return stop->token != ASH_TOKEN_EOF && token->kind == stop->token;
}

static bool ash_parser_at_end(struct ash_parser* parser) {
    struct ash_token* token = ash_parser_peek(parser);
    return token != NULL && token->kind == ASH_TOKEN_EOF;
}

static void ash_parser_skip_newlines(struct ash_parser* parser) {
    while (true) {
        struct ash_token* token = ash_parser_peek(parser);
        if (token == NULL || token->kind != ASH_TOKEN_NEWLINE) {
            return;
        }
        struct ash_token consumed;
        (void)ash_parser_take(parser, &consumed);
        ash_token_destroy(&consumed);
    }
}

static struct ash_ast* ash_parse_list(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
);

static bool ash_parser_consume_word(
    struct ash_parser* parser,
    const char* expected,
    const char* error
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }
    if (token->kind != ASH_TOKEN_WORD ||
        !ash_word_is_unquoted_literal(&token->word, expected)) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            token->location,
            error
        );
        return false;
    }
    struct ash_token consumed;
    (void)ash_parser_take(parser, &consumed);
    ash_token_destroy(&consumed);
    return true;
}

static bool ash_parser_take_redirection(
    struct ash_parser* parser,
    struct ash_redirection* redirection
) {
    *redirection = (struct ash_redirection){0};
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }

    char* io_number = NULL;
    if (token->kind == ASH_TOKEN_IO_NUMBER) {
        struct ash_token io;
        (void)ash_parser_take(parser, &io);
        struct ash_source_location io_location = io.location;
        io_number = io.io_number;
        io.io_number = NULL;
        ash_token_destroy(&io);

        char* end = NULL;
        errno = 0;
        long parsed = strtol(io_number, &end, 10);
        if (errno != 0 || end == io_number || *end != '\0' ||
            parsed < 0 || parsed > INT_MAX) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                io_location,
                "invalid redirection fd"
            );
            free(io_number);
            return false;
        }

        token = ash_parser_peek(parser);
        if (token == NULL) {
            free(io_number);
            return false;
        }
        if (!ash_token_is_redirection(token->kind)) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "redirection operator expected after IO number"
            );
            free(io_number);
            return false;
        }
    }
    else if (!ash_token_is_redirection(token->kind)) {
        return false;
    }

    struct ash_token operator;
    (void)ash_parser_take(parser, &operator);
    redirection->operator = operator.kind;
    redirection->io_number = io_number;
    redirection->location = operator.location;
    ash_token_destroy(&operator);

    token = ash_parser_peek(parser);
    if (token == NULL) {
        ash_redirection_destroy(redirection);
        return false;
    }
    if (token->kind == ASH_TOKEN_EOF || token->kind == ASH_TOKEN_NEWLINE) {
        ash_parser_fail(
            parser,
            ASH_PARSER_INCOMPLETE,
            redirection->location,
            "redirection target expected"
        );
        ash_redirection_destroy(redirection);
        return false;
    }
    if (token->kind != ASH_TOKEN_WORD) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            token->location,
            "redirection target must be a word"
        );
        ash_redirection_destroy(redirection);
        return false;
    }

    struct ash_token target;
    (void)ash_parser_take(parser, &target);
    redirection->target = target.word;
    target.word = (struct ash_word){0};
    ash_token_destroy(&target);
    return true;
}

static bool ash_parser_take_trailing_redirections(
    struct ash_parser* parser,
    struct ash_ast* node
) {
    while (true) {
        struct ash_token* token = ash_parser_peek(parser);
        if (token == NULL) {
            return false;
        }
        if (token->kind != ASH_TOKEN_IO_NUMBER &&
            !ash_token_is_redirection(token->kind)) {
            return true;
        }

        struct ash_redirection redirection;
        if (!ash_parser_take_redirection(parser, &redirection)) {
            return false;
        }
        if (ash_ast_add_trailing_redirection(node, &redirection) != 0) {
            ash_redirection_destroy(&redirection);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "out of memory"
            );
            return false;
        }
    }
}

static struct ash_ast* ash_parse_simple(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    struct ash_token* first = ash_parser_peek(parser);
    if (first == NULL) {
        return NULL;
    }
    struct ash_ast* node = ash_ast_create(ASH_AST_SIMPLE, first->location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, first->location, "out of memory");
        return NULL;
    }

    bool command_word_seen = false;
    while (!ash_parser_at_stop(parser, stop)) {
        struct ash_token* token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (token->kind == ASH_TOKEN_IO_NUMBER ||
            ash_token_is_redirection(token->kind)) {
            struct ash_redirection redirection;
            if (!ash_parser_take_redirection(parser, &redirection)) {
                ash_ast_destroy(node);
                return NULL;
            }
            if (ash_ast_simple_add_redirection(node, &redirection) != 0) {
                ash_redirection_destroy(&redirection);
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    token->location,
                    "out of memory"
                );
                ash_ast_destroy(node);
                return NULL;
            }
            continue;
        }
        if (token->kind != ASH_TOKEN_WORD) {
            break;
        }

        struct ash_token word_token;
        (void)ash_parser_take(parser, &word_token);
        bool assignment = !command_word_seen &&
            ash_word_is_assignment(&word_token.word);
        if (!assignment) {
            command_word_seen = true;
        }
        if (ash_ast_simple_add_word(node, &word_token.word, assignment) != 0) {
            ash_token_destroy(&word_token);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "out of memory"
            );
            ash_ast_destroy(node);
            return NULL;
        }
        ash_token_destroy(&word_token);
    }

    if (node->value.simple.count == 0u) {
        struct ash_token* token = ash_parser_peek(parser);
        const char* error = "command expected";
        if (token != NULL && token->kind != ASH_TOKEN_EOF) {
            error = "syntax error near unexpected token";
        }
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            (token != NULL) ? token->location : node->location,
            error
        );
        ash_ast_destroy(node);
        return NULL;
    }
    return node;
}

static struct ash_ast* ash_parse_group(
    struct ash_parser* parser,
    enum ash_ast_kind kind,
    struct ash_source_location location,
    struct ash_parse_stop stop
) {
    ash_parser_skip_newlines(parser);
    struct ash_ast* body = ash_parse_list(parser, &stop);
    if (body == NULL) {
        return NULL;
    }

    struct ash_token* closing = ash_parser_peek(parser);
    if (closing == NULL) {
        ash_ast_destroy(body);
        return NULL;
    }
    if (!ash_parser_at_stop(parser, &stop)) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            closing->location,
            (kind == ASH_AST_SUBSHELL) ?
                "closing parenthesis expected" : "closing brace expected"
        );
        ash_ast_destroy(body);
        return NULL;
    }

    struct ash_token consumed;
    (void)ash_parser_take(parser, &consumed);
    ash_token_destroy(&consumed);

    struct ash_ast* node = ash_ast_create(kind, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        ash_ast_destroy(body);
        return NULL;
    }
    node->value.group.body = body;
    if (!ash_parser_take_trailing_redirections(parser, node)) {
        ash_ast_destroy(node);
        return NULL;
    }
    return node;
}

static struct ash_ast* ash_parse_if_after_keyword(
    struct ash_parser* parser,
    struct ash_source_location location
) {
    struct ash_ast* condition = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {"then"},
            .require_separator = true,
        }
    );
    if (condition == NULL ||
        !ash_parser_consume_word(parser, "then", "'then' expected")) {
        ash_ast_destroy(condition);
        return NULL;
    }

    struct ash_ast* then_branch = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {"elif", "else", "fi"},
            .require_separator = true,
        }
    );
    if (then_branch == NULL) {
        ash_ast_destroy(condition);
        return NULL;
    }

    struct ash_ast* else_branch = NULL;
    struct ash_token* ending = ash_parser_peek(parser);
    if (ending == NULL) {
        ash_ast_destroy(condition);
        ash_ast_destroy(then_branch);
        return NULL;
    }
    if (ending->kind == ASH_TOKEN_WORD &&
        ash_word_is_unquoted_literal(&ending->word, "elif")) {
        struct ash_source_location elif_location = ending->location;
        struct ash_token keyword;
        (void)ash_parser_take(parser, &keyword);
        ash_token_destroy(&keyword);
        else_branch = ash_parse_if_after_keyword(parser, elif_location);
        if (else_branch == NULL) {
            ash_ast_destroy(condition);
            ash_ast_destroy(then_branch);
            return NULL;
        }
    }
    else {
        if (ending->kind == ASH_TOKEN_WORD &&
            ash_word_is_unquoted_literal(&ending->word, "else")) {
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            else_branch = ash_parse_list(
                parser,
                &(struct ash_parse_stop){
                    .words = {"fi"},
                    .require_separator = true,
                }
            );
            if (else_branch == NULL) {
                ash_ast_destroy(condition);
                ash_ast_destroy(then_branch);
                return NULL;
            }
        }
        if (!ash_parser_consume_word(parser, "fi", "'fi' expected")) {
            ash_ast_destroy(condition);
            ash_ast_destroy(then_branch);
            ash_ast_destroy(else_branch);
            return NULL;
        }
    }

    struct ash_ast* node = ash_ast_create(ASH_AST_IF, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        ash_ast_destroy(condition);
        ash_ast_destroy(then_branch);
        ash_ast_destroy(else_branch);
        return NULL;
    }
    node->value.conditional.condition = condition;
    node->value.conditional.then_branch = then_branch;
    node->value.conditional.else_branch = else_branch;
    return node;
}

static struct ash_ast* ash_parse_loop_after_keyword(
    struct ash_parser* parser,
    struct ash_source_location location,
    enum ash_ast_kind kind
) {
    struct ash_ast* condition = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {"do"},
            .require_separator = true,
        }
    );
    if (condition == NULL ||
        !ash_parser_consume_word(parser, "do", "'do' expected")) {
        ash_ast_destroy(condition);
        return NULL;
    }
    struct ash_ast* body = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {"done"},
            .require_separator = true,
        }
    );
    if (body == NULL ||
        !ash_parser_consume_word(parser, "done", "'done' expected")) {
        ash_ast_destroy(condition);
        ash_ast_destroy(body);
        return NULL;
    }

    struct ash_ast* node = ash_ast_create(kind, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        ash_ast_destroy(condition);
        ash_ast_destroy(body);
        return NULL;
    }
    node->value.loop.condition = condition;
    node->value.loop.body = body;
    if (!ash_parser_take_trailing_redirections(parser, node)) {
        ash_ast_destroy(node);
        return NULL;
    }
    return node;
}

static struct ash_ast* ash_parse_command(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    if (token->kind == ASH_TOKEN_LPAREN) {
        struct ash_source_location location = token->location;
        struct ash_token opening;
        (void)ash_parser_take(parser, &opening);
        ash_token_destroy(&opening);
        return ash_parse_group(
            parser,
            ASH_AST_SUBSHELL,
            location,
            (struct ash_parse_stop){
                .token = ASH_TOKEN_RPAREN,
                .require_separator = false,
            }
        );
    }
    if (token->kind == ASH_TOKEN_WORD &&
        ash_word_is_unquoted_literal(&token->word, "{")) {
        struct ash_source_location location = token->location;
        struct ash_token opening;
        (void)ash_parser_take(parser, &opening);
        ash_token_destroy(&opening);
        return ash_parse_group(
            parser,
            ASH_AST_BRACE_GROUP,
            location,
            (struct ash_parse_stop){
                .token = ASH_TOKEN_EOF,
                .words = {"}"},
                .require_separator = true,
            }
        );
    }
    if (token->kind == ASH_TOKEN_WORD &&
        ash_word_is_unquoted_literal(&token->word, "if")) {
        struct ash_source_location location = token->location;
        struct ash_token keyword;
        (void)ash_parser_take(parser, &keyword);
        ash_token_destroy(&keyword);
        struct ash_ast* node = ash_parse_if_after_keyword(parser, location);
        if (node != NULL &&
            !ash_parser_take_trailing_redirections(parser, node)) {
            ash_ast_destroy(node);
            return NULL;
        }
        return node;
    }
    if (token->kind == ASH_TOKEN_WORD &&
        (ash_word_is_unquoted_literal(&token->word, "while") ||
         ash_word_is_unquoted_literal(&token->word, "until"))) {
        enum ash_ast_kind kind =
            ash_word_is_unquoted_literal(&token->word, "while") ?
                ASH_AST_WHILE : ASH_AST_UNTIL;
        struct ash_source_location location = token->location;
        struct ash_token keyword;
        (void)ash_parser_take(parser, &keyword);
        ash_token_destroy(&keyword);
        return ash_parse_loop_after_keyword(parser, location, kind);
    }
    return ash_parse_simple(parser, stop);
}

static struct ash_ast* ash_parse_pipeline(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    struct ash_source_location location = token->location;
    bool negated = false;
    if (token->kind == ASH_TOKEN_WORD &&
        ash_word_is_unquoted_literal(&token->word, "!")) {
        struct ash_token bang;
        (void)ash_parser_take(parser, &bang);
        ash_token_destroy(&bang);
        negated = true;
        ash_parser_skip_newlines(parser);
    }

    struct ash_ast* node = ash_ast_create(ASH_AST_PIPELINE, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        return NULL;
    }
    node->value.pipeline.negated = negated;

    struct ash_ast* command = ash_parse_command(parser, stop);
    if (command == NULL ||
        ash_ast_pipeline_add(node, command, ASH_PIPE_STDOUT) != 0) {
        ash_ast_destroy(command);
        if (parser->result == ASH_PARSER_COMPLETE) {
            ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        }
        ash_ast_destroy(node);
        return NULL;
    }

    while (true) {
        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (token->kind != ASH_TOKEN_PIPE &&
            token->kind != ASH_TOKEN_PIPE_AND) {
            break;
        }
        enum ash_pipe_operator operator_before =
            (token->kind == ASH_TOKEN_PIPE_AND) ?
                ASH_PIPE_STDOUT_STDERR : ASH_PIPE_STDOUT;
        struct ash_source_location operator_location = token->location;
        struct ash_token operator;
        (void)ash_parser_take(parser, &operator);
        ash_token_destroy(&operator);
        ash_parser_skip_newlines(parser);

        command = ash_parse_command(parser, stop);
        if (command == NULL) {
            if (parser->result == ASH_PARSER_COMPLETE) {
                ash_parser_fail(
                    parser,
                    ash_parser_at_end(parser) ?
                        ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
                    operator_location,
                    "command expected after pipe"
                );
            }
            else if (parser->result == ASH_PARSER_INCOMPLETE) {
                parser->error_location = operator_location;
                parser->error = "command expected after pipe";
            }
            else if (parser->error != NULL &&
                     strcmp(
                         parser->error,
                         "syntax error near unexpected token"
                     ) == 0) {
                parser->error_location = operator_location;
                parser->error = "command expected after pipe";
            }
            ash_ast_destroy(node);
            return NULL;
        }
        if (ash_ast_pipeline_add(node, command, operator_before) != 0) {
            ash_ast_destroy(command);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                operator_location,
                "out of memory"
            );
            ash_ast_destroy(node);
            return NULL;
        }
    }
    return node;
}

static struct ash_ast* ash_parse_and_or(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    struct ash_ast* node = ash_ast_create(ASH_AST_AND_OR, token->location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, "out of memory");
        return NULL;
    }

    struct ash_ast* pipeline = ash_parse_pipeline(parser, stop);
    if (pipeline == NULL ||
        ash_ast_and_or_add(node, pipeline, ASH_AND_IF) != 0) {
        ash_ast_destroy(pipeline);
        if (parser->result == ASH_PARSER_COMPLETE) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "out of memory"
            );
        }
        ash_ast_destroy(node);
        return NULL;
    }

    while (true) {
        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (token->kind != ASH_TOKEN_AND_IF &&
            token->kind != ASH_TOKEN_OR_IF) {
            break;
        }
        enum ash_and_or_operator operator_before =
            (token->kind == ASH_TOKEN_AND_IF) ? ASH_AND_IF : ASH_OR_IF;
        struct ash_source_location operator_location = token->location;
        struct ash_token operator;
        (void)ash_parser_take(parser, &operator);
        ash_token_destroy(&operator);
        ash_parser_skip_newlines(parser);

        pipeline = ash_parse_pipeline(parser, stop);
        if (pipeline == NULL) {
            if (parser->result == ASH_PARSER_COMPLETE) {
                ash_parser_fail(
                    parser,
                    ash_parser_at_end(parser) ?
                        ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
                    operator_location,
                    "pipeline expected after AND-OR operator"
                );
            }
            else if (parser->result == ASH_PARSER_INCOMPLETE) {
                parser->error_location = operator_location;
                parser->error = "pipeline expected after AND-OR operator";
            }
            ash_ast_destroy(node);
            return NULL;
        }
        if (ash_ast_and_or_add(node, pipeline, operator_before) != 0) {
            ash_ast_destroy(pipeline);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                operator_location,
                "out of memory"
            );
            ash_ast_destroy(node);
            return NULL;
        }
    }
    return node;
}

static struct ash_ast* ash_parse_list(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    ash_parser_skip_newlines(parser);
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    struct ash_ast* node = ash_ast_create(ASH_AST_LIST, token->location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, "out of memory");
        return NULL;
    }

    while (!ash_parser_at_end(parser) && !ash_parser_at_stop(parser, stop)) {
        struct ash_ast* command = ash_parse_and_or(parser, stop);
        if (command == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (ash_ast_list_add(node, command) != 0) {
            ash_ast_destroy(command);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "out of memory"
            );
            ash_ast_destroy(node);
            return NULL;
        }

        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        bool had_separator = false;
        if (token->kind == ASH_TOKEN_AMP) {
            node->value.list.entries[node->value.list.count - 1u].asynchronous = true;
            struct ash_token separator;
            (void)ash_parser_take(parser, &separator);
            ash_token_destroy(&separator);
            had_separator = true;
        }
        else if (token->kind == ASH_TOKEN_SEMI ||
                 token->kind == ASH_TOKEN_NEWLINE) {
            do {
                struct ash_token separator;
                (void)ash_parser_take(parser, &separator);
                ash_token_destroy(&separator);
                had_separator = true;
                token = ash_parser_peek(parser);
            } while (token != NULL &&
                     (token->kind == ASH_TOKEN_SEMI ||
                      token->kind == ASH_TOKEN_NEWLINE));
        }

        if (ash_parser_at_end(parser) || ash_parser_at_stop(parser, stop)) {
            if (stop->require_separator && !had_separator) {
                token = ash_parser_peek(parser);
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    (token != NULL) ? token->location : node->location,
                    "separator expected before closing token"
                );
                ash_ast_destroy(node);
                return NULL;
            }
            break;
        }
        if (!had_separator) {
            token = ash_parser_peek(parser);
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                (token != NULL) ? token->location : node->location,
                "command separator expected"
            );
            ash_ast_destroy(node);
            return NULL;
        }
    }

    if (node->value.list.count == 0u) {
        token = ash_parser_peek(parser);
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            (token != NULL) ? token->location : node->location,
            "command expected"
        );
        ash_ast_destroy(node);
        return NULL;
    }
    return node;
}

void ash_parser_init(
    struct ash_parser* parser,
    const char* source_name,
    const char* input,
    size_t length
) {
    *parser = (struct ash_parser){
        .result = ASH_PARSER_COMPLETE,
    };
    ash_lexer_init(&parser->lexer, source_name, input, length);
}

void ash_parser_destroy(struct ash_parser* parser) {
    if (parser->has_lookahead) {
        ash_token_destroy(&parser->lookahead);
    }
    parser->has_lookahead = false;
}

enum ash_parser_result ash_parser_parse_program(
    struct ash_parser* parser,
    struct ash_ast** program
) {
    *program = NULL;
    ash_parser_skip_newlines(parser);
    struct ash_token* first = ash_parser_peek(parser);
    if (first == NULL) {
        return parser->result;
    }
    if (first->kind == ASH_TOKEN_EOF) {
        *program = ash_ast_create(ASH_AST_LIST, first->location);
        if (*program == NULL) {
            return ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                first->location,
                "out of memory"
            );
        }
        return ASH_PARSER_COMPLETE;
    }

    struct ash_parse_stop stop = {
        .token = ASH_TOKEN_EOF,
    };
    struct ash_ast* node = ash_parse_list(parser, &stop);
    if (node == NULL) {
        return parser->result;
    }

    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        ash_ast_destroy(node);
        return parser->result;
    }
    if (token->kind != ASH_TOKEN_EOF) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            token->location,
            "unexpected token after command list"
        );
        ash_ast_destroy(node);
        return parser->result;
    }

    *program = node;
    return ASH_PARSER_COMPLETE;
}
