#ifndef BX_LIB_GLOB_SYNTAX_H
#define BX_LIB_GLOB_SYNTAX_H

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
static inline const char* bx_glob_bracket_end_with_classes(const char* text,
                                                          const char* end,
                                                          bool glibc_classes) {
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
            if (glibc_classes && p[1] == ':') {
                const char* name_end = p + 2;
                while (name_end < end && *name_end >= 'a' && *name_end < 'z')
                    name_end++;
                if (end - name_end >= 2 && name_end[0] == ':' && name_end[1] == ']')
                    p = name_end + 1;
                continue;
            }
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

static inline const char* bx_glob_bracket_end(const char* text, const char* end) {
    return bx_glob_bracket_end_with_classes(text, end, false);
}

static inline const char* bx_glob_skip_member(const char* atom, const char* end,
                                              mbstate_t* state) {
    if (*atom == '\\')
        atom++;
    if (atom == end)
        return NULL;
    if (!state)
        return atom + 1;
    size_t len = mbrlen(atom, (size_t)(end - atom), state);
    if (len == 0 || len > (size_t)(end - atom) || !mbsinit(state))
        return NULL;
    return atom + len;
}

/* For literal fallback, the relevant subject byte is '['. glibc can
 * reach fallback through a matching '[' member before an unfinished range. */
static inline bool bx_glob_members_allow_literal(const char* atom, const char* end,
                                                 mbstate_t* state) {
    bool literal_open = false;
    while (atom < end) {
        const char* member = atom;
        atom = bx_glob_skip_member(atom, end, state);
        if (!atom)
            return false;
        bool is_open = *member == '[' || (*member == '\\' && member[1] == '[');
        if (atom < end && *atom == '-') {
            if (++atom == end)
                return literal_open || is_open;
            atom = bx_glob_skip_member(atom, end, state);
            if (!atom)
                return false;
        } else {
            literal_open |= is_open;
        }
    }
    return true;
}

static inline bool bx_glob_terminal_hyphen_allows_literal(const char* atom, const char* end) {
    if (!bx_glob_members_allow_literal(atom, end, NULL))
        return false;
    if (MB_CUR_MAX == 1)
        return true;

    /* fnmatch may use byte matching for an invalid multibyte filename.
     * Rewrite only when byte and multibyte parsing agree. */
    mbstate_t state = {0};
    int saved_errno = errno;
    bool matched = bx_glob_members_allow_literal(atom, end, &state);
    errno = saved_errno;
    return matched;
}

/* Requires one writable byte after the terminator. Quote the opening bracket
 * of a simple incomplete [^] expression, but leave ambiguous tails alone. */
static inline void bx_glob_normalize_caret(char* pattern) {
    char* end = pattern + strlen(pattern);
    for (char* p = pattern; p < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
        } else if (*p == '[' && p + 1 < end) {
            const char* after = bx_glob_bracket_end(p, end);
#ifdef __GLIBC__
            /* An invalid class name can leave its ']' available to
             * close the outer bracket; the shell uses its own grammar. */
            if (!after && p[1] == '^')
                after = bx_glob_bracket_end_with_classes(p, end, true);
#endif
            if (!after) {
                /* With no closing delimiter, a final [^- is literal.
                 * glibc's other caret interpretation sees an unfinished
                 * range instead, even after an earlier '[' fell back. */
                if (end - p >= 3 && memcmp(end - 3, "[^-", 3) == 0 &&
                    !strpbrk(p, "]\\") && !strstr(p, "[.") && !strstr(p, "[=")) {
                    memmove(end - 2, end - 3, 4);
                    end[-3] = '\\';
                    break;
                }
                if (end - p >= 3 && p[1] == '^' && p[2] == ']') {
                    for (const char* tail = p + 3; tail < end; tail++) {
                        if (*tail == '\\' && tail + 1 < end)
                            tail++;
                        else if (*tail == ']' ||
                                 (*tail == '[' && tail + 1 < end &&
                                  (strchr(".=", tail[1]) ||
                                   (tail[1] == ':' && end - tail >= CHARCLASS_NAME_MAX) ||
                                   (tail[1] == '^' && end[-1] == '-'))))
                            return;
                    }
                    const char* hyphen = strchr(p + 3, '-');
                    if (end[-1] != '-' ||
                        (end - p >= 5 && hyphen == end - 2 && !strchr(p + 3, '\\')) ||
                        bx_glob_terminal_hyphen_allows_literal(p + 2, end)) {
                        memmove(p + 1, p, (size_t)(end - p) + 1);
                        *p = '\\';
                    }
#ifdef __GLIBC__
                    /* glibc rejects a single terminal range. A prior
                     * hyphen could make this one a range endpoint instead. */
                    else if (hyphen == end - 1 && !strpbrk(p + 3, "[\\")) {
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
