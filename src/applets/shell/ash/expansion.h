#ifndef BX_APPLETS_SHELL_ASH_EXPANSION_H
#define BX_APPLETS_SHELL_ASH_EXPANSION_H

#include <stdbool.h>

struct ash_shell;
struct ash_word;

enum ash_expansion_context {
    ASH_EXPANSION_WORD = 0,
    ASH_EXPANSION_PATTERN,
};

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

#endif /* BX_APPLETS_SHELL_ASH_EXPANSION_H */
