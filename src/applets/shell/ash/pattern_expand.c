#include <stdlib.h>
#include <string.h>

#include "applets/shell/ash/expansion.h"
#include "applets/shell/ash/pattern.h"

enum ash_pattern_compile_result ash_pattern_compile_word(
    struct ash_shell* shell,
    const struct ash_word* word,
    const struct ash_pattern_options* options,
    struct ash_pattern* pattern
) {
    if (shell == NULL || word == NULL || pattern == NULL ||
        !ash_pattern_options_valid(options)) {
        return ASH_PATTERN_COMPILE_INVALID;
    }
    char* expanded = NULL;
    if (!ash_expand(
            shell,
            word,
            ASH_EXPANSION_PATTERN,
            &expanded
        )) {
        return ASH_PATTERN_COMPILE_EXPANSION_ERROR;
    }
    enum ash_pattern_compile_result result = ash_pattern_compile(
        expanded,
        strlen(expanded),
        options,
        pattern
    );
    free(expanded);
    return result;
}
