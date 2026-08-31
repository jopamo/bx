#ifndef BX_APPLETS_SHELL_ASH_AST_H
#define BX_APPLETS_SHELL_ASH_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "applets/shell/ash/lexer.h"
#include "applets/shell/ash/syntax.h"

enum ash_ast_kind {
    ASH_AST_SIMPLE = 0,
    ASH_AST_LIST,
    ASH_AST_AND_OR,
    ASH_AST_PIPELINE,
    ASH_AST_SUBSHELL,
    ASH_AST_BRACE_GROUP,
    ASH_AST_IF,
    ASH_AST_WHILE,
    ASH_AST_UNTIL,
    ASH_AST_FOR,
    ASH_AST_CASE,
};

enum ash_simple_item_kind {
    ASH_SIMPLE_WORD = 0,
    ASH_SIMPLE_ASSIGNMENT,
    ASH_SIMPLE_REDIRECTION,
};

struct ash_redirection {
    enum ash_token_kind operator;
    char* io_number;
    struct ash_word target;
    struct ash_source_location location;
};

struct ash_simple_item {
    enum ash_simple_item_kind kind;
    struct ash_source_location location;
    union {
        struct ash_word word;
        struct ash_redirection redirection;
    } value;
};

struct ash_ast;

struct ash_list_entry {
    struct ash_ast* command;
    bool asynchronous;
};

enum ash_and_or_operator {
    ASH_AND_IF = 0,
    ASH_OR_IF,
};

enum ash_pipe_operator {
    ASH_PIPE_STDOUT = 0,
    ASH_PIPE_STDOUT_STDERR,
};

struct ash_case_clause {
    struct ash_word* patterns;
    size_t pattern_count;
    size_t pattern_capacity;
    struct ash_ast* body;
};

struct ash_ast {
    enum ash_ast_kind kind;
    struct ash_source_location location;
    struct ash_redirection* trailing_redirections;
    size_t trailing_redirection_count;
    size_t trailing_redirection_capacity;

    union {
        struct {
            struct ash_simple_item* items;
            size_t count;
            size_t capacity;
        } simple;
        struct {
            struct ash_list_entry* entries;
            size_t count;
            size_t capacity;
        } list;
        struct {
            struct ash_ast** pipelines;
            enum ash_and_or_operator* operators;
            size_t count;
            size_t capacity;
        } and_or;
        struct {
            struct ash_ast** commands;
            enum ash_pipe_operator* operators;
            size_t count;
            size_t capacity;
            bool negated;
        } pipeline;
        struct {
            struct ash_ast* body;
        } group;
        struct {
            struct ash_ast* condition;
            struct ash_ast* then_branch;
            struct ash_ast* else_branch;
        } conditional;
        struct {
            struct ash_ast* condition;
            struct ash_ast* body;
        } loop;
        struct {
            char* name;
            struct ash_word* words;
            size_t word_count;
            size_t word_capacity;
            bool explicit_words;
            struct ash_ast* body;
        } for_loop;
        struct {
            struct ash_word subject;
            struct ash_case_clause* clauses;
            size_t clause_count;
            size_t clause_capacity;
        } case_command;
    } value;
};

struct ash_ast* ash_ast_create(
    enum ash_ast_kind kind,
    struct ash_source_location location
);
void ash_ast_destroy(struct ash_ast* node);

int ash_ast_simple_add_word(
    struct ash_ast* node,
    struct ash_word* word,
    bool assignment
);
int ash_ast_simple_add_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
);
int ash_ast_add_trailing_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
);
int ash_ast_list_add(struct ash_ast* node, struct ash_ast* command);
int ash_ast_and_or_add(
    struct ash_ast* node,
    struct ash_ast* pipeline,
    enum ash_and_or_operator operator_before
);
int ash_ast_pipeline_add(
    struct ash_ast* node,
    struct ash_ast* command,
    enum ash_pipe_operator operator_before
);
int ash_case_clause_add_pattern(
    struct ash_case_clause* clause,
    struct ash_word* pattern
);
int ash_ast_case_add_clause(
    struct ash_ast* node,
    struct ash_case_clause* clause
);

void ash_redirection_destroy(struct ash_redirection* redirection);
void ash_case_clause_destroy(struct ash_case_clause* clause);
bool ash_word_is_unquoted_literal(const struct ash_word* word, const char* text);
bool ash_word_is_assignment(const struct ash_word* word);

#endif /* BX_APPLETS_SHELL_ASH_AST_H */
