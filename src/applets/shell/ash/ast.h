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
    ASH_AST_FUNCTION,
    ASH_AST_ARITHMETIC_COMMAND,
    ASH_AST_CONDITIONAL_COMMAND,
    ASH_AST_C_STYLE_FOR,
    ASH_AST_SELECT,
    ASH_AST_TIME,
    ASH_AST_COPROC,
};

enum ash_simple_item_kind {
    ASH_SIMPLE_WORD = 0,
    ASH_SIMPLE_ASSIGNMENT,
    ASH_SIMPLE_REDIRECTION,
};

struct ash_ast;

enum ash_process_substitution_direction {
    ASH_PROCESS_SUBSTITUTION_READ,
    ASH_PROCESS_SUBSTITUTION_WRITE,
};

struct ash_process_substitution {
    enum ash_process_substitution_direction direction;
    /* Index of the ASH_WORD_PROCESS_SUBSTITUTION placeholder. */
    size_t part_index;
    struct ash_ast* command;
    struct ash_source_location location;
};

struct ash_ast_word {
    struct ash_word syntax;
    bool has_syntax;
    /* Sorted by part_index in lexical order for one-pass expansion. */
    struct ash_process_substitution* process_substitutions;
    size_t process_substitution_count;
    size_t process_substitution_capacity;
};

struct ash_redirection {
    enum ash_token_kind operator;
    /* Owned when non-NULL. */
    char* io_number;
    /* Owned structured target. */
    struct ash_ast_word target;
    struct ash_source_location location;
};

struct ash_simple_item {
    enum ash_simple_item_kind kind;
    struct ash_source_location location;
    union {
        struct ash_ast_word word;
        struct ash_redirection redirection;
    } value;
};

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

enum ash_case_terminator {
    ASH_CASE_TERMINATE = 0,
    ASH_CASE_FALL_THROUGH,
    ASH_CASE_TEST_NEXT,
};

struct ash_case_clause {
    struct ash_ast_word* patterns;
    size_t pattern_count;
    size_t pattern_capacity;
    struct ash_ast* body;
    enum ash_case_terminator terminator;
};

struct ash_arithmetic_expression {
    /* Owned exact expression text, excluding command delimiters. */
    char* text;
    size_t length;
    struct ash_source_location location;
};

enum ash_condition_kind {
    ASH_CONDITION_WORD,
    ASH_CONDITION_UNARY,
    ASH_CONDITION_BINARY,
    ASH_CONDITION_NOT,
    ASH_CONDITION_AND,
    ASH_CONDITION_OR,
    ASH_CONDITION_GROUP,
};

enum ash_condition_unary_operator {
    ASH_CONDITION_STRING_NONEMPTY,
    ASH_CONDITION_STRING_EMPTY,
    ASH_CONDITION_OPTION_ENABLED,
    ASH_CONDITION_VARIABLE_SET,
    ASH_CONDITION_NAMEREF,
    ASH_CONDITION_FILE_EXISTS,
    ASH_CONDITION_FILE_BLOCK,
    ASH_CONDITION_FILE_CHARACTER,
    ASH_CONDITION_FILE_DIRECTORY,
    ASH_CONDITION_FILE_REGULAR,
    ASH_CONDITION_FILE_SETGID,
    ASH_CONDITION_FILE_SYMLINK,
    ASH_CONDITION_FILE_STICKY,
    ASH_CONDITION_FILE_NAMED_PIPE,
    ASH_CONDITION_FILE_READABLE,
    ASH_CONDITION_FILE_NONEMPTY,
    ASH_CONDITION_FILE_TERMINAL,
    ASH_CONDITION_FILE_SETUID,
    ASH_CONDITION_FILE_WRITABLE,
    ASH_CONDITION_FILE_EXECUTABLE,
    ASH_CONDITION_FILE_OWNED_GROUP,
    ASH_CONDITION_FILE_OWNED_USER,
    ASH_CONDITION_FILE_MODIFIED,
    ASH_CONDITION_FILE_SOCKET,
};

enum ash_condition_binary_operator {
    ASH_CONDITION_STRING_EQUAL,
    ASH_CONDITION_STRING_NOT_EQUAL,
    ASH_CONDITION_STRING_PATTERN,
    ASH_CONDITION_STRING_NOT_PATTERN,
    ASH_CONDITION_STRING_REGEX,
    ASH_CONDITION_STRING_BEFORE,
    ASH_CONDITION_STRING_AFTER,
    ASH_CONDITION_ARITHMETIC_EQUAL,
    ASH_CONDITION_ARITHMETIC_NOT_EQUAL,
    ASH_CONDITION_ARITHMETIC_LESS,
    ASH_CONDITION_ARITHMETIC_LESS_EQUAL,
    ASH_CONDITION_ARITHMETIC_GREATER,
    ASH_CONDITION_ARITHMETIC_GREATER_EQUAL,
    ASH_CONDITION_FILE_NEWER,
    ASH_CONDITION_FILE_OLDER,
    ASH_CONDITION_FILE_SAME,
};

struct ash_condition {
    enum ash_condition_kind kind;
    struct ash_source_location location;
    union {
        struct ash_ast_word word;
        struct {
            enum ash_condition_unary_operator operator;
            struct ash_ast_word operand;
        } unary;
        struct {
            enum ash_condition_binary_operator operator;
            struct ash_ast_word left;
            struct ash_ast_word right;
        } binary;
        struct {
            struct ash_condition* left;
            struct ash_condition* right;
        } branches;
    } value;
};

enum ash_function_syntax {
    ASH_FUNCTION_POSIX,
    ASH_FUNCTION_KEYWORD,
    ASH_FUNCTION_KEYWORD_WITH_PARENS,
};

struct ash_ast {
    /*
     * An AST recursively owns every child pointer, word, redirection, name,
     * clause, and backing array reachable from this node.
     */
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
            struct ash_ast_word* words;
            size_t word_count;
            size_t word_capacity;
            bool explicit_words;
            struct ash_ast* body;
        } for_loop;
        struct {
            struct ash_ast_word subject;
            struct ash_case_clause* clauses;
            size_t clause_count;
            size_t clause_capacity;
        } case_command;
        struct {
            char* name;
            struct ash_ast* body;
            enum ash_function_syntax syntax;
        } function;
        struct {
            struct ash_arithmetic_expression expression;
        } arithmetic_command;
        struct {
            struct ash_condition* root;
        } conditional_command;
        struct {
            struct ash_arithmetic_expression initializer;
            struct ash_arithmetic_expression condition;
            struct ash_arithmetic_expression updater;
            struct ash_ast* body;
        } c_style_for;
        struct {
            struct ash_ast* pipeline;
            bool posix_format;
        } time_command;
        struct {
            /* Optional owned Bash coprocess name. */
            char* name;
            struct ash_ast* command;
        } coproc;
    } value;
};

struct ash_ast* ash_ast_create(
    enum ash_ast_kind kind,
    struct ash_source_location location
);
struct ash_ast* ash_ast_clone(const struct ash_ast* node);
void ash_ast_destroy(struct ash_ast* node);

void ash_ast_word_init(
    struct ash_ast_word* word,
    struct ash_source_location location
);
void ash_ast_word_destroy(struct ash_ast_word* word);
int ash_ast_word_clone(
    struct ash_ast_word* destination,
    const struct ash_ast_word* source
);
int ash_ast_word_take_syntax(
    struct ash_ast_word* destination,
    struct ash_word* syntax
);
int ash_ast_word_take_process_substitution(
    struct ash_ast_word* word,
    size_t part_index,
    enum ash_process_substitution_direction direction,
    struct ash_ast** command
);

int ash_arithmetic_expression_copy(
    struct ash_arithmetic_expression* expression,
    struct ash_source_location location,
    const char* text,
    size_t length
);
void ash_arithmetic_expression_destroy(
    struct ash_arithmetic_expression* expression
);

struct ash_condition* ash_condition_create(
    enum ash_condition_kind kind,
    struct ash_source_location location
);
struct ash_condition* ash_condition_clone(const struct ash_condition* condition);
void ash_condition_destroy(struct ash_condition* condition);
int ash_condition_take_word(
    struct ash_condition* condition,
    struct ash_word* word
);
int ash_condition_take_unary(
    struct ash_condition* condition,
    enum ash_condition_unary_operator operator,
    struct ash_word* operand
);
int ash_condition_take_binary(
    struct ash_condition* condition,
    enum ash_condition_binary_operator operator,
    struct ash_word* left,
    struct ash_word* right
);
int ash_condition_take_branches(
    struct ash_condition* condition,
    struct ash_condition** left,
    struct ash_condition** right
);

int ash_ast_simple_take_word(
    struct ash_ast* node,
    struct ash_word* word,
    bool assignment
);
int ash_ast_simple_take_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
);
int ash_ast_take_trailing_redirection(
    struct ash_ast* node,
    struct ash_redirection* redirection
);
int ash_ast_list_take(struct ash_ast* node, struct ash_ast** command);
int ash_ast_and_or_take(
    struct ash_ast* node,
    struct ash_ast** pipeline,
    enum ash_and_or_operator operator_before
);
int ash_ast_pipeline_take(
    struct ash_ast* node,
    struct ash_ast** command,
    enum ash_pipe_operator operator_before
);
int ash_ast_for_take_name(struct ash_ast* node, char** name);
int ash_ast_for_take_word(struct ash_ast* node, struct ash_word* word);
int ash_ast_for_take_body(
    struct ash_ast* node,
    struct ash_ast** body,
    bool explicit_words
);
int ash_ast_case_take_subject(
    struct ash_ast* node,
    struct ash_word* subject
);
int ash_case_clause_take_pattern(
    struct ash_case_clause* clause,
    struct ash_word* pattern
);
int ash_ast_case_take_clause(
    struct ash_ast* node,
    struct ash_case_clause* clause
);
int ash_ast_take_condition(
    struct ash_ast* node,
    struct ash_condition** condition
);
int ash_ast_arithmetic_command_copy_expression(
    struct ash_ast* node,
    struct ash_source_location location,
    const char* text,
    size_t length
);
int ash_ast_c_style_for_copy_expressions_take_body(
    struct ash_ast* node,
    struct ash_source_location location,
    const char* initializer,
    size_t initializer_length,
    const char* condition,
    size_t condition_length,
    const char* updater,
    size_t updater_length,
    struct ash_ast** body
);
int ash_ast_take_time_pipeline(
    struct ash_ast* node,
    struct ash_ast** pipeline,
    bool posix_format
);
int ash_ast_take_coproc_command(
    struct ash_ast* node,
    const char* name,
    struct ash_ast** command
);
int ash_ast_function_take(
    struct ash_ast* node,
    char** name,
    enum ash_function_syntax syntax,
    struct ash_ast** body
);

void ash_redirection_destroy(struct ash_redirection* redirection);
void ash_case_clause_destroy(struct ash_case_clause* clause);
bool ash_word_is_unquoted_literal(const struct ash_word* word, const char* text);
bool ash_word_is_assignment(const struct ash_word* word);

#endif /* BX_APPLETS_SHELL_ASH_AST_H */
