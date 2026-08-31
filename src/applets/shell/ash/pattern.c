#include <fnmatch.h>
#include <stdlib.h>

#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/pattern.h"

bool ash_pattern_matches(
    struct ash_shell* shell,
    const struct ash_word* pattern,
    const char* value,
    bool* matched
) {
    char* expanded = NULL;
    if (!ash_expand(
            shell,
            pattern,
            ASH_EXPANSION_PATTERN,
            &expanded
        )) {
        return false;
    }

    int result = fnmatch(expanded, value, 0);
    free(expanded);
    *matched = result == 0;
    return true;
}
