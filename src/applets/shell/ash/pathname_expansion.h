#ifndef BX_APPLETS_SHELL_ASH_PATHNAME_EXPANSION_H
#define BX_APPLETS_SHELL_ASH_PATHNAME_EXPANSION_H

#include <stdbool.h>
#include <stddef.h>

struct ash_pathname_matches {
    char** values;
    size_t count;
    size_t capacity;
};

enum ash_pathname_expansion_result {
    ASH_PATHNAME_EXPANSION_ERROR = -1,
    ASH_PATHNAME_EXPANSION_NO_MATCH = 0,
    ASH_PATHNAME_EXPANSION_MATCH,
};

/*
 * Patterns use the shell pattern compiler's backslash-quoted representation.
 * On MATCH, output owns a locale-sorted path vector. NO_MATCH leaves output
 * empty so the expansion policy adapter can preserve the original field.
 */
bool ash_pathname_pattern_may_expand(const char* pattern);
enum ash_pathname_expansion_result ash_pathname_expand(
    const char* pattern,
    struct ash_pathname_matches* output
);
void ash_pathname_matches_destroy(struct ash_pathname_matches* matches);

#endif /* BX_APPLETS_SHELL_ASH_PATHNAME_EXPANSION_H */
