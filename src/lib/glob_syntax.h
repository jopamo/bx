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

#ifdef __GLIBC__
enum {
    BX_GLOB_BRACKET_NONE = 0,
    BX_GLOB_BRACKET_CLOSED,
    BX_GLOB_BRACKET_FAIL
};

static inline bool bx_glob_class_name_valid(const char* name, size_t length) {
    static const char* const names[] = {
        "alnum", "alpha", "blank", "cntrl", "digit", "graph",
        "lower", "print", "punct", "space", "upper", "xdigit",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == length && memcmp(name, names[i], length) == 0)
            return true;
    }
    return false;
}

/* Range-aware bracket scan. A '[' after '-' is an endpoint, not a class.
 * Invalid [:name:] and [::] fail closed; a valid class does not end the bracket. */
static inline int bx_glob_scan_ranges(const char* text, const char* end, const char** after) {
    const char* p = text + 1;
    bool first = true;
    if (p < end && (*p == '!' || *p == '^'))
        p++;
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            p += 2;
            first = false;
            continue;
        }
        if (*p == ']' && !first) {
            *after = p + 1;
            return BX_GLOB_BRACKET_CLOSED;
        }
        if (*p == '[' && p + 1 < end && p[1] == ':') {
            const char* name = p + 2;
            if (name < end && *name == ':' && name + 1 < end && name[1] == ']')
                return BX_GLOB_BRACKET_FAIL;
            const char* q = name;
            while (q < end && *q >= 'a' && *q < 'z')
                q++;
            if (q + 1 < end && q[0] == ':' && q[1] == ']') {
                if (!bx_glob_class_name_valid(name, (size_t)(q - name)))
                    return BX_GLOB_BRACKET_FAIL;
                p = q + 2;
                first = false;
                continue;
            }
        } else if (*p == '[' && p + 1 < end && (p[1] == '.' || p[1] == '=')) {
            const char* next = bx_glob_notation_end(p, end);
            if (next) {
                p = next;
                first = false;
                continue;
            }
        }
        if (p + 2 < end && p[1] == '-' && p[2] != ']') {
            const char* ep = p + 2;
            if (*ep == '\\' && ep + 1 < end) {
                p = ep + 2;
                first = false;
                continue;
            }
            if (*ep == '[' && ep + 1 < end && ep[1] == '.') {
                const char* next = bx_glob_notation_end(ep, end);
                if (!next)
                    return BX_GLOB_BRACKET_NONE;
                p = next;
                first = false;
                continue;
            }
            p = ep + 1;
            first = false;
            continue;
        }
        p++;
        first = false;
    }
    return BX_GLOB_BRACKET_NONE;
}

static inline bool bx_glob_has_complete_notation(const char* text, const char* end) {
    for (const char* p = text; p + 1 < end; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            continue;
        }
        if (*p == '[' && (p[1] == '.' || p[1] == '=')) {
            if (bx_glob_notation_end(p, end) != NULL)
                return true;
        } else if (*p == '[' && p[1] == ':') {
            const char* name = p + 2;
            const char* q = name;
            while (q < end && *q >= 'a' && *q < 'z')
                q++;
            if (q + 1 < end && q[0] == ':' && q[1] == ']' &&
                bx_glob_class_name_valid(name, (size_t)(q - name)))
                return true;
        }
    }
    return false;
}

/* Quote incomplete [^] fallbacks whose strict parse would close early.
 * Skip forms that already agree: a terminal [^--, a short [^]-, an
 * unfinished collating start, and a few hyphen tails that are not literal. */
static inline bool bx_glob_should_quote_fallback(const char* p, const char* end) {
    size_t n = (size_t)(end - p);
    if (n < 4 || p[0] != '[' || p[1] != '^' || p[2] != ']')
        return false;
    if (bx_glob_has_complete_notation(p, end))
        return true;
    if (strstr(p, "[.") != NULL && strstr(p, ".]") == NULL)
        return false;
    if (n >= 4 && memcmp(end - 4, "[^--", 4) == 0)
        return false;
    if (n == 7 && memcmp(p, "[^]-", 4) == 0)
        return false;
    if (n >= 5 && memcmp(p, "[^]-", 4) == 0 && p[4] != '[')
        return false;
    if (n == 8 && memcmp(p, "[^][^-", 6) == 0 && p[6] != '[' && p[6] != '\\' && p[6] != '-')
        return false;
    if (n == 8 && memcmp(p, "[^][--", 6) == 0)
        return false;
    /* [^][-X-- matches itself under unset only when X collates at or
     * after '^'. Quoting any other X invents that literal match. */
    if (n == 8 && memcmp(p, "[^][-", 5) == 0 && p[6] == '-' && p[7] == '-') {
        char sample[2] = {p[5], '\0'};
        int saved = errno;
        errno = 0;
        int order = strcoll(sample, "^");
        int failed = errno != 0;
        errno = saved;
        if (failed || order < 0)
            return false;
    }
    if (n == 9 && memcmp(p, "[^]-[^-", 7) == 0 && p[7] != '[' && p[7] != '\\' && p[7] != '-')
        return false;
    return end[-1] == '-' || (n >= 2 && end[-2] == '\\' && end[-1] == '-');
}

/* Quote the opening '[' and a later '[^-'. Uses at most two spare bytes. */
static inline void bx_glob_quote_incomplete(char* p, char** end) {
    char* hyphen = NULL;
    for (char* t = p + 1; t + 2 < *end; t++) {
        if (*t == '\\' && t + 1 < *end) {
            t++;
            continue;
        }
        if (t[0] == '[' && t[1] == '^' && t[2] == '-')
            hyphen = t;
    }
    memmove(p + 1, p, (size_t)(*end - p) + 1);
    *p = '\\';
    (*end)++;
    if (hyphen != NULL) {
        hyphen++;
        memmove(hyphen + 1, hyphen, (size_t)(*end - hyphen) + 1);
        *hyphen = '\\';
        (*end)++;
    }
}

static inline bool bx_glob_bang_unclosed_carets(char* start, char* end) {
    bool changed = false;
    for (char* t = start; t + 1 < end; t++) {
        if (*t == '\\' && t + 1 < end) {
            t++;
            continue;
        }
        if (*t == '[' && t[1] == '^') {
            const char* after = NULL;
            if (bx_glob_scan_ranges(t, end, &after) != BX_GLOB_BRACKET_CLOSED) {
                t[1] = '!';
                changed = true;
            }
        }
    }
    return changed;
}
#endif

enum { BX_GLOB_CARET_SPARE = 2 };

/* Requires BX_GLOB_CARET_SPARE writable bytes after the terminator.
 * Quote simple incomplete caret expressions; leave ambiguous tails alone. */
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
            if (!after) {
                const char* ranged = NULL;
                int kind = bx_glob_scan_ranges(p, end, &ranged);
                if (kind == BX_GLOB_BRACKET_CLOSED)
                    after = ranged;
                else if (kind == BX_GLOB_BRACKET_FAIL && p[1] == '^') {
                    /* Invalid [:name:] fails the match; quoting would
                     * accept a literal fallback glibc never takes. */
                    p[1] = '!';
                    break;
                }
            }
#endif
            if (!after) {
                /* Without a closing bracket, these tails can be reached
                 * after an earlier '[' fell back to literal matching. */
                if (end - p >= 3 && end[-1] == '-' && !strpbrk(p, "]\\") &&
                    !strstr(p, "[.") && !strstr(p, "[=")) {
                    if (memcmp(end - 3, "[^-", 3) == 0) {
                        memmove(end - 2, end - 3, 4);
                        end[-3] = '\\';
                        break;
                    }
#ifdef __GLIBC__
                    /* Unlike [^-, the second '-' in [^-- starts an unfinished range. */
                    if (end - p >= 4 && memcmp(end - 4, "[^--", 4) == 0) {
                        end[-3] = '!';
                        break;
                    }
#endif
                }
                if (end - p >= 3 && p[1] == '^' && p[2] == ']') {
                    char* nested = NULL;
                    for (const char* tail = p + 3; tail < end; tail++) {
                        if (*tail == '\\' && tail + 1 < end) {
                            tail++;
                        } else if (end - tail == 3 && memcmp(tail, "[^-", 3) == 0) {
                            nested = end - 3;
                        } else if (*tail == ']' ||
                                   (*tail == '[' && tail + 1 < end &&
                                    (strchr(".=", tail[1]) ||
                                     (tail[1] == ':' && end - tail >= CHARCLASS_NAME_MAX) ||
                                     (tail[1] == '^' && end[-1] == '-')))) {
#ifdef __GLIBC__
                            if (bx_glob_should_quote_fallback(p, end)) {
                                bx_glob_quote_incomplete(p, &end);
                                return;
                            }
#endif
                            return;
                        }
                    }
                    const char* hyphen = strchr(p + 3, '-');
                    if (end[-1] != '-' ||
                        (end - p >= 5 && hyphen == end - 2 && !strchr(p + 3, '\\')) ||
                        bx_glob_terminal_hyphen_allows_literal(p + 2, end)) {
                        if (nested) {
                            memmove(nested + 1, nested, 4);
                            *nested = '\\';
                            end++;
                        }
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
#ifdef __GLIBC__
                /* [[^-X- falls back and reparses a failing range. X is not
                 * itself a bracket, escape, or another hyphen. */
                if (bx_glob_should_quote_fallback(p, end)) {
                    bx_glob_quote_incomplete(p, &end);
                    return;
                }
                if (end[-1] == '-' && end - p >= 5 && memcmp(end - 5, "[^-", 3) == 0 &&
                    end[-2] != '[' && end[-2] != '\\' && end[-2] != '-' &&
                    !(p[1] == '^' && p[2] == ']'))
                    bx_glob_bang_unclosed_carets(p, end);
#endif
                break;
            }
            if (p[1] == '^')
                p[1] = '!';
            p += after - p - 1;
        }
    }
}

#endif
