#ifndef BX_LIB_GLOB_SYNTAX_H
#define BX_LIB_GLOB_SYNTAX_H

#include <stddef.h>
#include <string.h>

/* Return past the closing delimiter of a bracket atom such as [:alpha:]. */
static inline const char* bx_glob_notation_end(const char* text, const char* end) {
    for (const char* p = text + 2; end - p >= 2; p++) {
        if (*p == '\\')
            p++;
        else if (p[0] == text[1] && p[1] == ']')
            return p + 2;
    }
    return NULL;
}

/* text points at '['. ']' is literal as the first member after negation. */
static inline const char* bx_glob_bracket_end(const char* text, const char* end) {
    const char* p = text + 1;
    unsigned missing_notation = 0;
    if (p < end && (*p == '!' || *p == '^'))
        p++;
    if (p < end && *p == ']')
        p++;
    for (; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
        } else if (*p == ']') {
            return p + 1;
        } else if (*p == '[' && p + 1 < end &&
                   (p[1] == '.' || p[1] == '=' || p[1] == ':')) {
            unsigned bit = p[1] == '.' ? 1u : p[1] == '=' ? 2u : 4u;
            if (!(missing_notation & bit)) {
                const char* next = bx_glob_notation_end(p, end);
                if (next)
                    p = next - 1;
                else
                    missing_notation |= bit;
            }
        }
    }
    return NULL;
}

/* Spare storage after the terminator lets us quote the opening bracket of
 * a simple incomplete [^] expression. Leave ambiguous bracket tails alone. */
static inline void bx_glob_normalize_caret_with_spare(char* pattern, size_t spare) {
    const char* end = pattern + strlen(pattern);
    for (char* p = pattern; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
        } else if (*p == '[' && p + 1 < end) {
            const char* after = bx_glob_bracket_end(p, end);
            if (!after) {
                if (spare && end - p >= 3 && p[1] == '^' && p[2] == ']' &&
                    !strpbrk(p + 3, "[]\\-")) {
                    memmove(p + 1, p, (size_t)(end - p) + 1);
                    *p = '\\';
                }
                break;
            }
            if (p[1] == '^')
                p[1] = '!';
            p += after - p - 1;
        }
    }
}

static inline void bx_glob_normalize_caret(char* pattern) {
    bx_glob_normalize_caret_with_spare(pattern, 0);
}

#endif
