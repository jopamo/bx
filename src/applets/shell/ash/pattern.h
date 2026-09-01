#ifndef BX_APPLETS_SHELL_ASH_PATTERN_H
#define BX_APPLETS_SHELL_ASH_PATTERN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ash_shell;
struct ash_word;

enum ash_pattern_purpose {
    ASH_PATTERN_CASE,
    ASH_PATTERN_PATHNAME_EXPANSION,
    ASH_PATTERN_PARAMETER_REMOVE,
    ASH_PATTERN_PARAMETER_SUBSTITUTE,
    ASH_PATTERN_CONDITIONAL,
    ASH_PATTERN_GLOBIGNORE,
    ASH_PATTERN_COMPLETION,
};

enum ash_pattern_domain {
    ASH_PATTERN_STRING,
    ASH_PATTERN_PATHNAME,
};

enum ash_pattern_flag {
    ASH_PATTERN_EXTGLOB = 1u << 0,
    ASH_PATTERN_GLOBSTAR = 1u << 1,
    ASH_PATTERN_CASE_INSENSITIVE = 1u << 2,
    ASH_PATTERN_MATCH_DOTFILES = 1u << 3,
    ASH_PATTERN_ASCII_RANGES = 1u << 4,
    ASH_PATTERN_FLAG_ALL = (1u << 5) - 1u,
};

/*
 * Compilation is recursive only for extglob expressions. Keep the limit part
 * of the contract so every future backend rejects the same hostile input
 * before consuming the C stack without bound.
 */
#define ASH_PATTERN_MAX_NESTING 64u

struct ash_pattern_options {
    enum ash_pattern_purpose purpose;
    enum ash_pattern_domain domain;
    uint32_t flags;
};

enum ash_pattern_term_kind {
    ASH_PATTERN_LITERAL,
    ASH_PATTERN_ANY_STRING,
    ASH_PATTERN_ANY_CHARACTER,
    ASH_PATTERN_BRACKET,
    ASH_PATTERN_PATH_SEPARATOR,
    ASH_PATTERN_GLOBSTAR_TERM,
    ASH_PATTERN_EXTGLOB_TERM,
};

enum ash_extglob_operator {
    ASH_EXTGLOB_ZERO_OR_ONE,
    ASH_EXTGLOB_ZERO_OR_MORE,
    ASH_EXTGLOB_ONE_OR_MORE,
    ASH_EXTGLOB_ONE_OF,
    ASH_EXTGLOB_NONE_OF,
};

struct ash_pattern_span {
    char* text;
    size_t length;
};

struct ash_pattern_expression;

struct ash_pattern_term {
    enum ash_pattern_term_kind kind;
    size_t source_offset;
    union {
        struct ash_pattern_span span;
        struct {
            enum ash_extglob_operator operator;
            struct ash_pattern_expression* expression;
        } extglob;
    } value;
};

struct ash_pattern_sequence {
    struct ash_pattern_term* terms;
    size_t count;
    size_t capacity;
};

struct ash_pattern_expression {
    struct ash_pattern_sequence* alternatives;
    size_t count;
    size_t capacity;
};

enum ash_pattern_compile_result {
    ASH_PATTERN_COMPILE_OK,
    ASH_PATTERN_COMPILE_INVALID,
    ASH_PATTERN_COMPILE_LIMIT,
    ASH_PATTERN_COMPILE_NO_MEMORY,
    ASH_PATTERN_COMPILE_EXPANSION_ERROR,
};

enum ash_pattern_match_result {
    ASH_PATTERN_MATCH,
    ASH_PATTERN_NO_MATCH,
    ASH_PATTERN_MATCH_UNSUPPORTED,
    ASH_PATTERN_MATCH_ERROR,
};

enum ash_pattern_required_feature {
    ASH_PATTERN_REQUIRES_EXTGLOB = 1u << 0,
    ASH_PATTERN_REQUIRES_GLOBSTAR = 1u << 1,
    ASH_PATTERN_REQUIRES_ASCII_RANGES = 1u << 2,
};

struct ash_pattern {
    struct ash_pattern_options options;
    struct ash_pattern_sequence root;
    /*
     * Owned expanded source is retained for diagnostics and the current
     * compatibility matcher. The recursive IR is canonical for consumers.
     */
    char* source;
    size_t source_length;
    uint32_t required_features;
};

bool ash_pattern_options_valid(const struct ash_pattern_options* options);

/*
 * Cold-path compilers. On success, pattern owns the resulting IR and source
 * snapshot until ash_pattern_destroy(). The destination must be zero-
 * initialized or previously destroyed. On failure it is left unchanged.
 *
 * ash_pattern_compile_word() is the shell-policy adapter: it expands the
 * structured word while preserving quoted metacharacters, then delegates to
 * the policy-free compiler.
 */
enum ash_pattern_compile_result ash_pattern_compile(
    const char* expanded_pattern,
    size_t length,
    const struct ash_pattern_options* options,
    struct ash_pattern* pattern
);
enum ash_pattern_compile_result ash_pattern_compile_word(
    struct ash_shell* shell,
    const struct ash_word* word,
    const struct ash_pattern_options* options,
    struct ash_pattern* pattern
);

/*
 * Allocation-free hot path. Matching observes only the immutable compiled
 * pattern, explicit options, and borrowed NUL-terminated candidate.
 */
enum ash_pattern_match_result ash_pattern_match(
    const struct ash_pattern* pattern,
    const char* value
);
void ash_pattern_destroy(struct ash_pattern* pattern);

#endif /* BX_APPLETS_SHELL_ASH_PATTERN_H */
