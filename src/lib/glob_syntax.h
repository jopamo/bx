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

/* A final hyphen is literal when escaped or consumed as a range member.
 * Only classify ASCII here: a multibyte endpoint may span several bytes. */
static inline int bx_glob_terminal_hyphen_is_member(const char* atom, const char* end) {
    while (atom < end) {
        if (*atom == '\\')
            atom++;
        if (atom == end || (unsigned char)*atom++ >= 0x80u)
            return 0;
        if (atom < end && *atom == '-') {
            atom++;
            if (atom < end && *atom == '\\')
                atom++;
            if (atom == end || (unsigned char)*atom++ >= 0x80u)
                return 0;
        }
    }
    return 1;
}

/* Requires one writable byte after the terminator. Quote the opening bracket
 * of a simple incomplete [^] expression, but leave ambiguous tails alone. */
static inline void bx_glob_normalize_caret(char* pattern) {
    const char* end = pattern + strlen(pattern);
    for (char* p = pattern; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
        } else if (*p == '[' && p + 1 < end) {
            const char* after = bx_glob_bracket_end(p, end);
            if (!after) {
                if (end - p >= 3 && p[1] == '^' && p[2] == ']') {
                    for (const char* tail = p + 3; tail < end; tail++) {
                        if (*tail == '\\' && tail + 1 < end)
                            tail++;
                        else if (*tail == '[' || *tail == ']')
                            return;
                    }
                    const char* hyphen = strchr(p + 3, '-');
                    if (end[-1] != '-' ||
                        (end - p >= 5 && hyphen == end - 2 && !strchr(p + 3, '\\')) ||
                        bx_glob_terminal_hyphen_is_member(p + 2, end)) {
                        memmove(p + 1, p, (size_t)(end - p) + 1);
                        *p = '\\';
                    }
#ifdef __GLIBC__
                    /* glibc rejects a single terminal range. A prior
                     * hyphen could make this one a range endpoint instead. */
                    else if (hyphen == end - 1 && !strchr(p + 3, '\\')) {
                        p[1] = '!';
                    }
#endif
                }
                break;
            }
            if (p[1] == '^')
                p[1] = '!';
            p += after - p - 1;
        }
    }
}

#endif
