#ifndef BX_APPLETS_SHELL_ASH_PATTERN_H
#define BX_APPLETS_SHELL_ASH_PATTERN_H

#include <stdbool.h>

struct ash_shell;
struct ash_word;

/*
 * Shell pattern matching for case and parameter operators. Pathname-specific
 * restrictions belong in a separate call path when pathname expansion lands.
 */
bool ash_pattern_matches(
    struct ash_shell* shell,
    const struct ash_word* pattern,
    const char* value,
    bool* matched
);

#endif /* BX_APPLETS_SHELL_ASH_PATTERN_H */
