#ifndef BX_APPLETS_SHELL_ASH_EXPANSION_H
#define BX_APPLETS_SHELL_ASH_EXPANSION_H

#include <stdbool.h>
#include <stddef.h>

struct ash_shell;
struct ash_word;

enum ash_expansion_context {
    ASH_EXPANSION_WORD = 0,
    ASH_EXPANSION_PATTERN,
};

struct ash_expanded_fields {
    /* Owns the values array and every NUL-terminated field. */
    char** values;
    size_t count;
    size_t capacity;
};

enum ash_redirection_expansion_result {
    ASH_REDIRECTION_EXPANSION_ERROR = 0,
    ASH_REDIRECTION_EXPANSION_OK,
    ASH_REDIRECTION_EXPANSION_AMBIGUOUS,
};

void ash_expanded_fields_init(struct ash_expanded_fields* fields);
void ash_expanded_fields_destroy(struct ash_expanded_fields* fields);
/*
 * Produces ordinary argument fields through quote-aware splitting and
 * pathname expansion. Noglob suppresses only the final pathname stage.
 */
bool ash_expand_argument(
    struct ash_shell* shell,
    const struct ash_word* word,
    struct ash_expanded_fields* fields
);

/*
 * Expands one structured word without field splitting or pathname expansion.
 * The caller owns the returned string. Context-specific field production is
 * added here rather than in the lexer or executor.
 */
bool ash_expand(
    struct ash_shell* shell,
    const struct ash_word* word,
    enum ash_expansion_context context,
    char** output
);
bool ash_expand_word(
    struct ash_shell* shell,
    const struct ash_word* word,
    char** output
);
/*
 * Redirection words do not field-split. Zero pathname matches preserve the
 * literal operand, one selects that path, and multiple matches are ambiguous.
 */
enum ash_redirection_expansion_result ash_expand_redirection(
    struct ash_shell* shell,
    const struct ash_word* word,
    char** output
);

#endif /* BX_APPLETS_SHELL_ASH_EXPANSION_H */
