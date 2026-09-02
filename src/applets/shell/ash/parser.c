#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/parser.h"
#include "applets/shell/ash/parser_internal.h"
#include "applets/shell/ash/variables.h"

enum ash_reserved_word {
    ASH_RESERVED_NONE = 0,
    ASH_RESERVED_BANG,
    ASH_RESERVED_LBRACE,
    ASH_RESERVED_RBRACE,
    ASH_RESERVED_CASE,
    ASH_RESERVED_DO,
    ASH_RESERVED_DONE,
    ASH_RESERVED_ELIF,
    ASH_RESERVED_ELSE,
    ASH_RESERVED_ESAC,
    ASH_RESERVED_FI,
    ASH_RESERVED_FOR,
    ASH_RESERVED_IF,
    ASH_RESERVED_IN,
    ASH_RESERVED_THEN,
    ASH_RESERVED_UNTIL,
    ASH_RESERVED_WHILE,
};

struct ash_reserved_word_entry {
    enum ash_reserved_word word;
    const char* spelling;
    size_t length;
};

#define ASH_RESERVED_ENTRY(word, spelling) \
    {word, spelling, sizeof(spelling) - 1u}
static const struct ash_reserved_word_entry ash_reserved_words[] = {
    ASH_RESERVED_ENTRY(ASH_RESERVED_BANG, "!"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_LBRACE, "{"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_RBRACE, "}"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_CASE, "case"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_DO, "do"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_DONE, "done"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_ELIF, "elif"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_ELSE, "else"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_ESAC, "esac"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_FI, "fi"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_FOR, "for"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_IF, "if"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_IN, "in"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_THEN, "then"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_UNTIL, "until"),
    ASH_RESERVED_ENTRY(ASH_RESERVED_WHILE, "while"),
};
#undef ASH_RESERVED_ENTRY

struct ash_parse_stop {
    enum ash_token_kind token;
    enum ash_reserved_word words[4];
    bool require_separator;
};

/*
 * The lexer deliberately emits every spelling as ASH_TOKEN_WORD. Classify a
 * word only when the parser is at a grammar position that admits reserved
 * words. The bounded copy scans structured word provenance once and allocates
 * nothing on the parse hot path.
 */
static enum ash_reserved_word ash_parser_reserved_word(
    const struct ash_token* token
) {
    if (token == NULL || token->kind != ASH_TOKEN_WORD) {
        return ASH_RESERVED_NONE;
    }

    char spelling[sizeof("until")];
    size_t length = 0u;
    for (size_t i = 0u; i < token->word.count; i++) {
        const struct ash_word_part* part = &token->word.parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part) ||
            part->length > sizeof(spelling) - 1u - length) {
            return ASH_RESERVED_NONE;
        }
        memcpy(spelling + length, part->text, part->length);
        length += part->length;
    }

    for (size_t i = 0u;
         i < sizeof(ash_reserved_words) /
             sizeof(ash_reserved_words[0]);
         i++) {
        const struct ash_reserved_word_entry* entry =
            &ash_reserved_words[i];
        if (entry->length == length &&
            memcmp(entry->spelling, spelling, length) == 0) {
            return entry->word;
        }
    }
    return ASH_RESERVED_NONE;
}

enum ash_parser_result ash_parser_fail(
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

static bool ash_parser_token_is_reserved(
    const struct ash_token* token
) {
    return ash_parser_reserved_word(token) != ASH_RESERVED_NONE;
}

static bool ash_parser_at_stop(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    if (!ash_parser_prepare_command_alias(
            parser,
            ash_parser_token_is_reserved
        )) {
        return false;
    }
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }
    enum ash_reserved_word word = ash_parser_reserved_word(token);
    if (word != ASH_RESERVED_NONE) {
        for (size_t i = 0u;
             i < sizeof(stop->words) / sizeof(stop->words[0]);
             i++) {
            if (stop->words[i] == word) {
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
static struct ash_ast* ash_parse_command(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
);

static bool ash_parser_consume_reserved(
    struct ash_parser* parser,
    enum ash_reserved_word expected,
    const char* error
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }
    if (ash_parser_reserved_word(token) != expected) {
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

    if (ash_token_is_redirection_prefix(token->kind)) {
        struct ash_token io;
        (void)ash_parser_take(parser, &io);
        struct ash_source_location io_location = io.location;
        redirection->prefix = (struct ash_redirection_prefix){
            .kind = io.kind == ASH_TOKEN_IO_NUMBER ?
                ASH_REDIRECTION_PREFIX_NUMBER :
                ASH_REDIRECTION_PREFIX_VARIABLE,
            .text = io.io_redirect,
            .location = io_location,
        };
        io.io_redirect = NULL;
        ash_token_destroy(&io);

        if (redirection->prefix.kind ==
            ASH_REDIRECTION_PREFIX_NUMBER) {
            char* end = NULL;
            errno = 0;
            long parsed = strtol(
                redirection->prefix.text,
                &end,
                10
            );
            if (errno != 0 ||
                end == redirection->prefix.text ||
                *end != '\0' ||
                parsed < 0 ||
                parsed > INT_MAX) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    io_location,
                    "invalid redirection fd"
                );
                ash_redirection_destroy(redirection);
                return false;
            }
        }

        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_redirection_destroy(redirection);
            return false;
        }
        if (!ash_token_is_redirection(token->kind)) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "redirection operator expected after IO prefix"
            );
            ash_redirection_destroy(redirection);
            return false;
        }
    }
    else if (!ash_token_is_redirection(token->kind)) {
        return false;
    }

    struct ash_token operator;
    (void)ash_parser_take(parser, &operator);
    redirection->operator = operator.kind;
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
    if (ash_ast_word_take_syntax(
            &redirection->target,
            &target.word
        ) != 0) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            target.location,
            "out of memory"
        );
        ash_token_destroy(&target);
        ash_redirection_destroy(redirection);
        return false;
    }
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
        if (!ash_token_is_redirection_prefix(token->kind) &&
            !ash_token_is_redirection(token->kind)) {
            return true;
        }

        struct ash_redirection redirection;
        if (!ash_parser_take_redirection(parser, &redirection)) {
            return false;
        }
        if (ash_ast_take_trailing_redirection(node, &redirection) != 0) {
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

static char* ash_parser_word_name(const struct ash_word* word) {
    size_t length = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        const struct ash_word_part* part = &word->parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part) ||
            part->length > SIZE_MAX - length) {
            return NULL;
        }
        length += part->length;
    }
    char* name = malloc(length + 1u);
    if (name == NULL) {
        return NULL;
    }
    size_t offset = 0u;
    for (size_t i = 0u; i < word->count; i++) {
        memcpy(name + offset, word->parts[i].text, word->parts[i].length);
        offset += word->parts[i].length;
    }
    name[offset] = '\0';
    if (!ash_is_valid_name_span(name, length)) {
        free(name);
        return NULL;
    }
    return name;
}

static struct ash_ast* ash_parse_simple(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    (void)stop;
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
    while (true) {
        if (!ash_parser_prepare_alias(
                parser,
                !command_word_seen,
                false,
                NULL
            )) {
            ash_ast_destroy(node);
            return NULL;
        }
        struct ash_token* token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (ash_token_is_redirection_prefix(token->kind) ||
            ash_token_is_redirection(token->kind)) {
            struct ash_redirection redirection;
            if (!ash_parser_take_redirection(parser, &redirection)) {
                ash_ast_destroy(node);
                return NULL;
            }
            if (ash_ast_simple_take_redirection(node, &redirection) != 0) {
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
        if (ash_ast_simple_take_word(
                node,
                &word_token.word,
                assignment
            ) != 0) {
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

        token = ash_parser_peek(parser);
        if (node->value.simple.count == 1u && !assignment &&
            token != NULL && token->kind == ASH_TOKEN_LPAREN) {
            char* function_name = ash_parser_word_name(
                &node->value.simple.items[0].value.word.syntax
            );
            if (function_name == NULL) {
                continue;
            }

            struct ash_token opening;
            (void)ash_parser_take(parser, &opening);
            ash_token_destroy(&opening);
            token = ash_parser_peek(parser);
            if (token == NULL || token->kind != ASH_TOKEN_RPAREN) {
                ash_parser_fail(
                    parser,
                    token != NULL && token->kind != ASH_TOKEN_EOF ?
                        ASH_PARSER_ERROR : ASH_PARSER_INCOMPLETE,
                    token != NULL ? token->location : node->location,
                    "')' expected in function definition"
                );
                free(function_name);
                ash_ast_destroy(node);
                return NULL;
            }
            struct ash_token closing;
            (void)ash_parser_take(parser, &closing);
            ash_token_destroy(&closing);
            ash_parser_skip_newlines(parser);

            struct ash_ast* body = ash_parse_command(parser, stop);
            if (body == NULL) {
                free(function_name);
                ash_ast_destroy(node);
                return NULL;
            }
            if (body->kind == ASH_AST_SIMPLE ||
                body->kind == ASH_AST_FUNCTION) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    body->location,
                    "compound command expected after function name"
                );
                free(function_name);
                ash_ast_destroy(body);
                ash_ast_destroy(node);
                return NULL;
            }

            struct ash_ast* function = ash_ast_create(
                ASH_AST_FUNCTION,
                node->location
            );
            if (function == NULL) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    node->location,
                    "out of memory"
                );
                free(function_name);
                ash_ast_destroy(body);
                ash_ast_destroy(node);
                return NULL;
            }
            if (ash_ast_function_take(
                    function,
                    &function_name,
                    ASH_FUNCTION_POSIX,
                    &body
                ) != 0) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    node->location,
                    "out of memory"
                );
                free(function_name);
                ash_ast_destroy(body);
                ash_ast_destroy(function);
                ash_ast_destroy(node);
                return NULL;
            }
            ash_ast_destroy(node);
            return function;
        }
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

static bool ash_parser_take_case_pattern(
    struct ash_parser* parser,
    struct ash_case_clause* clause
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return false;
    }
    if (token->kind != ASH_TOKEN_WORD) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            token->location,
            "case pattern expected"
        );
        return false;
    }

    struct ash_token pattern;
    (void)ash_parser_take(parser, &pattern);
    if (ash_case_clause_take_pattern(clause, &pattern.word) != 0) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            pattern.location,
            "out of memory"
        );
        ash_token_destroy(&pattern);
        return false;
    }
    ash_token_destroy(&pattern);
    return true;
}

static struct ash_ast* ash_parser_empty_list(
    struct ash_parser* parser,
    struct ash_source_location location
) {
    struct ash_ast* list = ash_ast_create(ASH_AST_LIST, location);
    if (list == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
    }
    return list;
}

static struct ash_ast* ash_parse_case_after_keyword(
    struct ash_parser* parser,
    struct ash_source_location location
) {
    struct ash_ast* node = ash_ast_create(ASH_AST_CASE, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        return NULL;
    }

    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        ash_ast_destroy(node);
        return NULL;
    }
    if (token->kind != ASH_TOKEN_WORD) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            token->location,
            "word expected after 'case'"
        );
        ash_ast_destroy(node);
        return NULL;
    }
    struct ash_token subject;
    (void)ash_parser_take(parser, &subject);
    if (ash_ast_case_take_subject(node, &subject.word) != 0) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            subject.location,
            "out of memory"
        );
        ash_token_destroy(&subject);
        ash_ast_destroy(node);
        return NULL;
    }
    ash_token_destroy(&subject);

    ash_parser_skip_newlines(parser);
    if (!ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_IN,
            "'in' expected"
        )) {
        ash_ast_destroy(node);
        return NULL;
    }
    ash_parser_skip_newlines(parser);

    while (true) {
        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_ast_destroy(node);
            return NULL;
        }
        if (ash_parser_reserved_word(token) == ASH_RESERVED_ESAC) {
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            break;
        }
        if (token->kind == ASH_TOKEN_EOF) {
            ash_parser_fail(
                parser,
                ASH_PARSER_INCOMPLETE,
                token->location,
                "'esac' expected"
            );
            ash_ast_destroy(node);
            return NULL;
        }

        struct ash_case_clause clause = {0};
        if (token->kind == ASH_TOKEN_LPAREN) {
            struct ash_token opening;
            (void)ash_parser_take(parser, &opening);
            ash_token_destroy(&opening);
        }
        if (!ash_parser_take_case_pattern(parser, &clause)) {
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }
        while (true) {
            token = ash_parser_peek(parser);
            if (token == NULL) {
                ash_case_clause_destroy(&clause);
                ash_ast_destroy(node);
                return NULL;
            }
            if (token->kind != ASH_TOKEN_PIPE) {
                break;
            }
            struct ash_token separator;
            (void)ash_parser_take(parser, &separator);
            ash_token_destroy(&separator);
            if (!ash_parser_take_case_pattern(parser, &clause)) {
                ash_case_clause_destroy(&clause);
                ash_ast_destroy(node);
                return NULL;
            }
        }
        token = ash_parser_peek(parser);
        if (token == NULL || token->kind != ASH_TOKEN_RPAREN) {
            ash_parser_fail(
                parser,
                token != NULL && token->kind != ASH_TOKEN_EOF ?
                    ASH_PARSER_ERROR : ASH_PARSER_INCOMPLETE,
                token != NULL ? token->location : location,
                "')' expected after case pattern"
            );
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }
        struct ash_token closing;
        (void)ash_parser_take(parser, &closing);
        struct ash_source_location body_location = closing.location;
        ash_token_destroy(&closing);
        ash_parser_skip_newlines(parser);

        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }
        bool empty = token->kind == ASH_TOKEN_DSEMI ||
            ash_parser_reserved_word(token) == ASH_RESERVED_ESAC;
        clause.body = empty ?
            ash_parser_empty_list(parser, body_location) :
            ash_parse_list(
                parser,
                &(struct ash_parse_stop){
                    .token = ASH_TOKEN_DSEMI,
                    .words = {ASH_RESERVED_ESAC},
                }
            );
        if (clause.body == NULL) {
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }

        token = ash_parser_peek(parser);
        if (token == NULL) {
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }
        if (token->kind == ASH_TOKEN_DSEMI) {
            struct ash_token terminator;
            (void)ash_parser_take(parser, &terminator);
            ash_token_destroy(&terminator);
            ash_parser_skip_newlines(parser);
        }
        if (ash_ast_case_take_clause(node, &clause) != 0) {
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                body_location,
                "out of memory"
            );
            ash_case_clause_destroy(&clause);
            ash_ast_destroy(node);
            return NULL;
        }
    }

    if (!ash_parser_take_trailing_redirections(parser, node)) {
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
            .words = {ASH_RESERVED_THEN},
            .require_separator = true,
        }
    );
    if (condition == NULL ||
        !ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_THEN,
            "'then' expected"
        )) {
        ash_ast_destroy(condition);
        return NULL;
    }

    struct ash_ast* then_branch = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {
                ASH_RESERVED_ELIF,
                ASH_RESERVED_ELSE,
                ASH_RESERVED_FI,
            },
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
    enum ash_reserved_word ending_word =
        ash_parser_reserved_word(ending);
    if (ending_word == ASH_RESERVED_ELIF) {
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
        if (ending_word == ASH_RESERVED_ELSE) {
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            else_branch = ash_parse_list(
                parser,
                &(struct ash_parse_stop){
                    .words = {ASH_RESERVED_FI},
                    .require_separator = true,
                }
            );
            if (else_branch == NULL) {
                ash_ast_destroy(condition);
                ash_ast_destroy(then_branch);
                return NULL;
            }
        }
        if (!ash_parser_consume_reserved(
                parser,
                ASH_RESERVED_FI,
                "'fi' expected"
            )) {
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
            .words = {ASH_RESERVED_DO},
            .require_separator = true,
        }
    );
    if (condition == NULL ||
        !ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_DO,
            "'do' expected"
        )) {
        ash_ast_destroy(condition);
        return NULL;
    }
    struct ash_ast* body = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {ASH_RESERVED_DONE},
            .require_separator = true,
        }
    );
    if (body == NULL ||
        !ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_DONE,
            "'done' expected"
        )) {
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

static char* ash_parser_take_name(
    struct ash_parser* parser,
    const char* error
) {
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    if (token->kind != ASH_TOKEN_WORD) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            token->location,
            error
        );
        return NULL;
    }

    size_t length = 0u;
    for (size_t i = 0u; i < token->word.count; i++) {
        const struct ash_word_part* part = &token->word.parts[i];
        if (part->kind != ASH_WORD_TEXT ||
            ash_word_part_is_quoted(part) ||
            part->length > SIZE_MAX - length) {
            ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, error);
            return NULL;
        }
        length += part->length;
    }
    if (length == SIZE_MAX) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, "out of memory");
        return NULL;
    }

    char* name = malloc(length + 1u);
    if (name == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, "out of memory");
        return NULL;
    }
    size_t offset = 0u;
    for (size_t i = 0u; i < token->word.count; i++) {
        const struct ash_word_part* part = &token->word.parts[i];
        memcpy(name + offset, part->text, part->length);
        offset += part->length;
    }
    name[offset] = '\0';
    if (!ash_is_valid_name_span(name, length)) {
        free(name);
        ash_parser_fail(parser, ASH_PARSER_ERROR, token->location, error);
        return NULL;
    }

    struct ash_token consumed;
    (void)ash_parser_take(parser, &consumed);
    ash_token_destroy(&consumed);
    return name;
}

static struct ash_ast* ash_parse_for_after_keyword(
    struct ash_parser* parser,
    struct ash_source_location location
) {
    struct ash_ast* node = ash_ast_create(ASH_AST_FOR, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        return NULL;
    }
    char* name = ash_parser_take_name(
        parser,
        "valid name expected after 'for'"
    );
    if (name == NULL ||
        ash_ast_for_take_name(node, &name) != 0) {
        free(name);
        ash_ast_destroy(node);
        return NULL;
    }

    bool explicit_words = false;
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        ash_ast_destroy(node);
        return NULL;
    }
    if (ash_parser_reserved_word(token) == ASH_RESERVED_IN) {
        struct ash_token keyword;
        (void)ash_parser_take(parser, &keyword);
        ash_token_destroy(&keyword);
        explicit_words = true;

        while (true) {
            token = ash_parser_peek(parser);
            if (token == NULL) {
                ash_ast_destroy(node);
                return NULL;
            }
            if (token->kind == ASH_TOKEN_SEMI ||
                token->kind == ASH_TOKEN_NEWLINE) {
                break;
            }
            if (token->kind != ASH_TOKEN_WORD) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    token->location,
                    "word or separator expected in for list"
                );
                ash_ast_destroy(node);
                return NULL;
            }
            struct ash_token word;
            (void)ash_parser_take(parser, &word);
            if (ash_ast_for_take_word(node, &word.word) != 0) {
                ash_parser_fail(
                    parser,
                    ASH_PARSER_ERROR,
                    word.location,
                    "out of memory"
                );
                ash_token_destroy(&word);
                ash_ast_destroy(node);
                return NULL;
            }
            ash_token_destroy(&word);
        }
    }

    token = ash_parser_peek(parser);
    if (token == NULL) {
        ash_ast_destroy(node);
        return NULL;
    }
    if (token->kind != ASH_TOKEN_SEMI &&
        token->kind != ASH_TOKEN_NEWLINE) {
        ash_parser_fail(
            parser,
            ash_parser_at_end(parser) ? ASH_PARSER_INCOMPLETE : ASH_PARSER_ERROR,
            token->location,
            "separator expected before 'do'"
        );
        ash_ast_destroy(node);
        return NULL;
    }
    do {
        struct ash_token separator;
        (void)ash_parser_take(parser, &separator);
        ash_token_destroy(&separator);
        token = ash_parser_peek(parser);
    } while (token != NULL &&
             (token->kind == ASH_TOKEN_SEMI ||
              token->kind == ASH_TOKEN_NEWLINE));

    if (!ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_DO,
            "'do' expected"
        )) {
        ash_ast_destroy(node);
        return NULL;
    }
    struct ash_ast* body = ash_parse_list(
        parser,
        &(struct ash_parse_stop){
            .words = {ASH_RESERVED_DONE},
            .require_separator = true,
        }
    );
    if (body == NULL ||
        !ash_parser_consume_reserved(
            parser,
            ASH_RESERVED_DONE,
            "'done' expected"
        )) {
        ash_ast_destroy(body);
        ash_ast_destroy(node);
        return NULL;
    }
    if (ash_ast_for_take_body(node, &body, explicit_words) != 0) {
        ash_parser_fail(
            parser,
            ASH_PARSER_ERROR,
            location,
            "out of memory"
        );
        ash_ast_destroy(body);
        ash_ast_destroy(node);
        return NULL;
    }
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
    if (!ash_parser_prepare_command_alias(
            parser,
            ash_parser_token_is_reserved
        )) {
        return NULL;
    }
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
    enum ash_reserved_word word = ash_parser_reserved_word(token);
    switch (word) {
        case ASH_RESERVED_LBRACE: {
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
                    .words = {ASH_RESERVED_RBRACE},
                    .require_separator = true,
                }
            );
        }
        case ASH_RESERVED_IF: {
            struct ash_source_location location = token->location;
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            struct ash_ast* node =
                ash_parse_if_after_keyword(parser, location);
            if (node != NULL &&
                !ash_parser_take_trailing_redirections(parser, node)) {
                ash_ast_destroy(node);
                return NULL;
            }
            return node;
        }
        case ASH_RESERVED_WHILE:
        case ASH_RESERVED_UNTIL: {
            enum ash_ast_kind kind =
                word == ASH_RESERVED_WHILE ?
                    ASH_AST_WHILE : ASH_AST_UNTIL;
            struct ash_source_location location = token->location;
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            return ash_parse_loop_after_keyword(
                parser,
                location,
                kind
            );
        }
        case ASH_RESERVED_FOR: {
            struct ash_source_location location = token->location;
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            return ash_parse_for_after_keyword(parser, location);
        }
        case ASH_RESERVED_CASE: {
            struct ash_source_location location = token->location;
            struct ash_token keyword;
            (void)ash_parser_take(parser, &keyword);
            ash_token_destroy(&keyword);
            return ash_parse_case_after_keyword(parser, location);
        }
        case ASH_RESERVED_NONE:
            return ash_parse_simple(parser, stop);
        case ASH_RESERVED_BANG:
        case ASH_RESERVED_RBRACE:
        case ASH_RESERVED_DO:
        case ASH_RESERVED_DONE:
        case ASH_RESERVED_ELIF:
        case ASH_RESERVED_ELSE:
        case ASH_RESERVED_ESAC:
        case ASH_RESERVED_FI:
        case ASH_RESERVED_IN:
        case ASH_RESERVED_THEN:
            ash_parser_fail(
                parser,
                ASH_PARSER_ERROR,
                token->location,
                "syntax error near unexpected token"
            );
            return NULL;
    }
    return NULL;
}

static struct ash_ast* ash_parse_pipeline(
    struct ash_parser* parser,
    const struct ash_parse_stop* stop
) {
    if (!ash_parser_prepare_command_alias(
            parser,
            ash_parser_token_is_reserved
        )) {
        return NULL;
    }
    struct ash_token* token = ash_parser_peek(parser);
    if (token == NULL) {
        return NULL;
    }
    struct ash_source_location location = token->location;
    bool negated = false;
    while (ash_parser_reserved_word(token) == ASH_RESERVED_BANG) {
        struct ash_token bang;
        (void)ash_parser_take(parser, &bang);
        ash_token_destroy(&bang);
        negated = !negated;
        ash_parser_skip_newlines(parser);
        if (!ash_parser_prepare_command_alias(
                parser,
                ash_parser_token_is_reserved
            )) {
            return NULL;
        }
        token = ash_parser_peek(parser);
        if (token == NULL) {
            return NULL;
        }
    }

    struct ash_ast* node = ash_ast_create(ASH_AST_PIPELINE, location);
    if (node == NULL) {
        ash_parser_fail(parser, ASH_PARSER_ERROR, location, "out of memory");
        return NULL;
    }
    node->value.pipeline.negated = negated;

    struct ash_ast* command = ash_parse_command(parser, stop);
    if (command == NULL ||
        ash_ast_pipeline_take(node, &command, ASH_PIPE_STDOUT) != 0) {
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
        if (ash_ast_pipeline_take(
                node,
                &command,
                operator_before
            ) != 0) {
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
        ash_ast_and_or_take(node, &pipeline, ASH_AND_IF) != 0) {
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
        if (ash_ast_and_or_take(
                node,
                &pipeline,
                operator_before
            ) != 0) {
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
        if (ash_ast_list_take(node, &command) != 0) {
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

void ash_parser_init_at_with_aliases(
    struct ash_parser* parser,
    struct ash_source_location origin,
    const char* input,
    size_t length,
    const struct ash_alias_table* aliases
) {
    *parser = (struct ash_parser){
        .result = ASH_PARSER_COMPLETE,
    };
    ash_parser_alias_state_init(parser, aliases);
    ash_lexer_init_at(&parser->lexer, origin, input, length);
}

void ash_parser_init_at(
    struct ash_parser* parser,
    struct ash_source_location origin,
    const char* input,
    size_t length
) {
    ash_parser_init_at_with_aliases(
        parser,
        origin,
        input,
        length,
        NULL
    );
}

void ash_parser_init(
    struct ash_parser* parser,
    const char* source_name,
    const char* input,
    size_t length
) {
    ash_parser_init_at(
        parser,
        (struct ash_source_location){
            .source = source_name != NULL ? source_name : "<input>",
            .line = 1u,
            .column = 1u,
        },
        input,
        length
    );
}

void ash_parser_destroy(struct ash_parser* parser) {
    ash_parser_alias_state_destroy(parser);
}

enum ash_parser_result ash_parser_parse_program(
    struct ash_parser* parser,
    struct ash_ast** program
) {
    *program = NULL;
    ash_parser_skip_newlines(parser);
    if (!ash_parser_prepare_command_alias(
            parser,
            ash_parser_token_is_reserved
        )) {
        return parser->result;
    }
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
