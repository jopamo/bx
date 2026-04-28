#include <regex.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "dev_counters.h"
#include "literal.h"
#include "pcre2_matcher.h"
#include "rg_text.h"
#include "search.h"
#include "search_internal.h"

enum matcher_kind {
    MATCHER_REGEX,
    MATCHER_POSIX,
    MATCHER_LITERAL,
    MATCHER_LITERAL_SET,
};

struct bx_literal_set {
    struct bx_literal_matcher **items;
    size_t count;
};

struct bx_matcher {
    enum matcher_kind kind;
    union {
        struct bx_regex *regex;
        struct bx_literal_matcher *literal;
        struct bx_literal_set literal_set;
        regex_t posix;
    };
};

static char *bx_regex_strerror_dup(int rc, const regex_t *regex) {
    size_t needed = regerror(rc, regex, NULL, 0);
    char *buf = malloc(needed > 0 ? needed : 1u);

    if (!buf)
        return NULL;
    regerror(rc, regex, buf, needed > 0 ? needed : 1u);
    return buf;
}

static bool rg_pattern_requires_pcre2(const char *pattern, const struct search_opts *opts) {
    if (!pattern)
        return false;

    if (opts && (opts->multiline || opts->multiline_dotall))
        return true;

    for (const char *p = pattern; *p; ++p) {
        if (*p == '\\') {
            ++p;
            if (!*p)
                break;
            if (*p >= '1' && *p <= '9')
                return true;
            if (*p == 'g' || *p == 'k')
                return true;
            continue;
        }

        if (*p != '(' || p[1] != '?')
            continue;

        if (p[2] == '=' || p[2] == '!' || p[2] == '>')
            return true;
        if (p[2] == '<' && (p[3] == '=' || p[3] == '!'))
            return true;
        if (p[2] == '(' || p[2] == 'R' || p[2] == '&')
            return true;
    }

    return false;
}

static bool pattern_is_plain_literal(const char *pattern) {
    if (!pattern || !*pattern)
        return false;

    for (const unsigned char *p = (const unsigned char *)pattern; *p; ++p) {
        switch (*p) {
        case '\\':
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
            return false;
        default:
            break;
        }
    }

    return true;
}

static bool search_pattern_ere_backrefs(const char *pattern,
                                        bool *has_backrefs_out,
                                        bool *invalid_backref_out) {
    bool in_class = false;
    bool has_backrefs = false;
    bool invalid_backref = false;
    size_t group_count = 0u;

    if (!pattern) {
        if (has_backrefs_out)
            *has_backrefs_out = false;
        if (invalid_backref_out)
            *invalid_backref_out = false;
        return false;
    }

    for (size_t i = 0u; pattern[i] != '\0'; ++i) {
        if (pattern[i] == '\\') {
            char next = pattern[i + 1u];

            if (next == '\0')
                break;
            if (in_class) {
                i++;
                continue;
            }
            if (next >= '1' && next <= '9') {
                has_backrefs = true;
                if ((size_t)(next - '0') > group_count)
                    invalid_backref = true;
            }
            i++;
            continue;
        }

        if (in_class) {
            if (pattern[i] == ']')
                in_class = false;
            continue;
        }

        if (pattern[i] == '[') {
            in_class = true;
            continue;
        }
        if (pattern[i] == '(') {
            group_count++;
            continue;
        }
    }

    if (has_backrefs_out)
        *has_backrefs_out = has_backrefs;
    if (invalid_backref_out)
        *invalid_backref_out = invalid_backref;
    return has_backrefs;
}

static size_t search_collation_token_end(const char *pattern,
                                         size_t start,
                                         char marker) {
    size_t cursor = start;

    while (pattern[cursor] != '\0') {
        if (pattern[cursor] == marker &&
            pattern[cursor + 1u] == ']' &&
            pattern[cursor + 2u] == ']') {
            return cursor;
        }
        cursor++;
    }
    return SIZE_MAX;
}

static bool search_append_pattern_bytes(char **buf,
                                        size_t *len,
                                        size_t *cap,
                                        const char *data,
                                        size_t data_len) {
    if (!buf || !len || !cap || (!data && data_len != 0u))
        return false;

    if (*len + data_len + 1u > *cap) {
        size_t next_cap = *cap ? *cap : 32u;

        while (*len + data_len + 1u > next_cap)
            next_cap *= 2u;
        char *grown = realloc(*buf, next_cap);
        if (!grown)
            return false;
        *buf = grown;
        *cap = next_cap;
    }

    if (data_len > 0u)
        memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
    return true;
}

static bool search_append_singleton_bracket(char **buf,
                                            size_t *len,
                                            size_t *cap,
                                            char ch) {
    if (ch == ']')
        return search_append_pattern_bytes(buf, len, cap, "[]]", 3u);
    return search_append_pattern_bytes(buf, len, cap, "[", 1u) &&
           search_append_pattern_bytes(buf, len, cap, &ch, 1u) &&
           search_append_pattern_bytes(buf, len, cap, "]", 1u);
}

static char *search_rewrite_simple_collation_tokens(const char *pattern, char **errmsg) {
    char *rewritten = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    size_t cursor = 0u;

    if (!pattern)
        return NULL;

    while (pattern[cursor] != '\0') {
        if (pattern[cursor] == '\\' && pattern[cursor + 1u] != '\0') {
            if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                             pattern + cursor, 2u)) {
                free(rewritten);
                return NULL;
            }
            cursor += 2u;
            continue;
        }

        if (pattern[cursor] == '[' &&
            pattern[cursor + 1u] == '[' &&
            (pattern[cursor + 2u] == '=' || pattern[cursor + 2u] == '.')) {
            char marker = pattern[cursor + 2u];
            size_t token_end = search_collation_token_end(pattern, cursor + 3u, marker);

            if (token_end == SIZE_MAX)
                break;
            if (token_end != cursor + 4u) {
                if (errmsg && !*errmsg)
                    *errmsg = strdup("Invalid collation character");
                free(rewritten);
                return NULL;
            }
            if (!search_append_singleton_bracket(&rewritten, &len, &cap,
                                                 pattern[cursor + 3u])) {
                free(rewritten);
                return NULL;
            }
            cursor = token_end + 3u;
            continue;
        }

        if (!search_append_pattern_bytes(&rewritten, &len, &cap, pattern + cursor, 1u)) {
            free(rewritten);
            return NULL;
        }
        cursor++;
    }

    if (pattern[cursor] != '\0') {
        if (!search_append_pattern_bytes(&rewritten, &len, &cap, pattern + cursor,
                                         strlen(pattern + cursor))) {
            free(rewritten);
            return NULL;
        }
    }

    return rewritten;
}

static bool search_append_warning_line(char **warnings,
                                       size_t *len,
                                       size_t *cap,
                                       const char *message) {
    return search_append_pattern_bytes(warnings, len, cap, message, strlen(message)) &&
           search_append_pattern_bytes(warnings, len, cap, "\n", 1u);
}

static bool search_is_gnu_bre_mode(enum bx_search_personality personality,
                                   const struct search_opts *opts) {
    return opts != NULL
        && personality != BX_SEARCH_RG
        && !opts->extended_regex
        && !opts->perl_regexp
        && !opts->fixed_strings;
}

static size_t search_bre_interval_close(const char *pattern, size_t start) {
    size_t cursor = start;

    while (pattern[cursor] != '\0') {
        if (pattern[cursor] == '\\' && pattern[cursor + 1u] == '}')
            return cursor;
        cursor++;
    }
    return SIZE_MAX;
}

static size_t search_bre_bracket_close(const char *pattern, size_t start) {
    size_t cursor = start;
    bool first = true;

    while (pattern[cursor] != '\0') {
        if (pattern[cursor] == '\\' && pattern[cursor + 1u] != '\0') {
            cursor += 2u;
            first = false;
            continue;
        }
        if (pattern[cursor] == ']' && !first)
            return cursor;
        if (pattern[cursor] != '^' || !first)
            first = false;
        cursor++;
    }
    return SIZE_MAX;
}

static char *search_rewrite_gnu_bre_escapes(const char *pattern,
                                            char **warningmsg,
                                            char **errmsg) {
    char *rewritten = NULL;
    char *warnings = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    size_t warn_len = 0u;
    size_t warn_cap = 0u;
    size_t cursor = 0u;
    bool prev_is_atom = false;
    size_t group_depth = 0u;

    if (!pattern)
        return NULL;

    while (pattern[cursor] != '\0') {
        if (pattern[cursor] == '[') {
            size_t bracket_end = search_bre_bracket_close(pattern, cursor + 1u);

            if (bracket_end == SIZE_MAX)
                break;
            if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                             pattern + cursor,
                                             bracket_end + 1u - cursor)) {
                free(warnings);
                free(rewritten);
                return NULL;
            }
            prev_is_atom = true;
            cursor = bracket_end + 1u;
            continue;
        }

        if (pattern[cursor] == '\\') {
            char next = pattern[cursor + 1u];

            if (next == '\0') {
                if (errmsg && !*errmsg)
                    *errmsg = strdup("Trailing backslash");
                free(warnings);
                free(rewritten);
                return NULL;
            }

            if (next == '+' || next == '?') {
                if (prev_is_atom) {
                    if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                                     pattern + cursor, 2u)) {
                        free(warnings);
                        free(rewritten);
                        return NULL;
                    }
                } else {
                    const char *warning = (next == '+')
                                              ? "warning: stray \\ before +"
                                              : "warning: stray \\ before ?";
                    if (!search_append_warning_line(&warnings, &warn_len, &warn_cap, warning) ||
                        !search_append_pattern_bytes(&rewritten, &len, &cap, &next, 1u)) {
                        free(warnings);
                        free(rewritten);
                        return NULL;
                    }
                }
                prev_is_atom = true;
                cursor += 2u;
                continue;
            }

            if (next == '{') {
                if (!prev_is_atom) {
                    if (!search_append_warning_line(&warnings, &warn_len, &warn_cap,
                                                    "warning: stray \\ before {") ||
                        !search_append_pattern_bytes(&rewritten, &len, &cap, "{", 1u)) {
                        free(warnings);
                        free(rewritten);
                        return NULL;
                    }
                    prev_is_atom = true;
                    cursor += 2u;
                    continue;
                }

                size_t close = search_bre_interval_close(pattern, cursor + 2u);
                if (close == SIZE_MAX) {
                    if (errmsg && !*errmsg)
                        *errmsg = strdup("Unmatched \\{");
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }

                if (!search_append_pattern_bytes(&rewritten, &len, &cap, "\\{", 2u)) {
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                if (pattern[cursor + 2u] == ',') {
                    if (!search_append_pattern_bytes(&rewritten, &len, &cap, "0", 1u)) {
                        free(warnings);
                        free(rewritten);
                        return NULL;
                    }
                }
                if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                                 pattern + cursor + 2u,
                                                 close - (cursor + 2u)) ||
                    !search_append_pattern_bytes(&rewritten, &len, &cap, "\\}", 2u)) {
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                prev_is_atom = true;
                cursor = close + 2u;
                continue;
            }

            if (next == '|' ) {
                if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                                 pattern + cursor, 2u)) {
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                prev_is_atom = false;
                cursor += 2u;
                continue;
            }

            if (next == '(') {
                if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                                 pattern + cursor, 2u)) {
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                group_depth++;
                prev_is_atom = false;
                cursor += 2u;
                continue;
            }

            if (next == ')') {
                if (group_depth == 0u) {
                    if (errmsg && !*errmsg)
                        *errmsg = strdup("Unmatched ) or \\)");
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                                 pattern + cursor, 2u)) {
                    free(warnings);
                    free(rewritten);
                    return NULL;
                }
                group_depth--;
                prev_is_atom = true;
                cursor += 2u;
                continue;
            }

            if (!search_append_pattern_bytes(&rewritten, &len, &cap,
                                             pattern + cursor, 2u)) {
                free(warnings);
                free(rewritten);
                return NULL;
            }
            prev_is_atom = (next != '|'
                            && next != '<'
                            && next != '>'
                            && next != '`'
                            && next != '\'');
            cursor += 2u;
            continue;
        }

        if (!search_append_pattern_bytes(&rewritten, &len, &cap, pattern + cursor, 1u)) {
            free(warnings);
            free(rewritten);
            return NULL;
        }
        prev_is_atom = pattern[cursor] != '^' && pattern[cursor] != '$';
        cursor++;
    }

    if (pattern[cursor] != '\0') {
        if (!search_append_pattern_bytes(&rewritten, &len, &cap, pattern + cursor,
                                         strlen(pattern + cursor))) {
            free(warnings);
            free(rewritten);
            return NULL;
        }
    }

    if (group_depth != 0u) {
        if (errmsg && !*errmsg)
            *errmsg = strdup("Unmatched ( or \\(");
        free(warnings);
        free(rewritten);
        return NULL;
    }

    if (warningmsg)
        *warningmsg = warnings;
    else
        free(warnings);
    return rewritten;
}

static bool matcher_uses_posix(const char *pattern,
                               enum bx_search_personality personality,
                               const struct search_opts *opts) {
    if (opts->fixed_strings)
        return false;

    if (personality != BX_SEARCH_RG)
        return !opts->perl_regexp;

    switch (opts->rg_engine) {
    case BX_RG_ENGINE_DEFAULT:
        return true;
    case BX_RG_ENGINE_AUTO:
        return !rg_pattern_requires_pcre2(pattern, opts);
    case BX_RG_ENGINE_PCRE2:
    case BX_RG_ENGINE_UNSPECIFIED:
    default:
        return false;
    }
}

static int matcher_find_posix_portable(regex_t *regex,
                                       const unsigned char *buf,
                                       size_t len,
                                       size_t start,
                                       struct bx_match *out) {
    if (!regex || !buf || !out || start > len)
        return -1;

#ifdef REG_STARTEND
    regmatch_t match = {
        .rm_so = (regoff_t)start,
        .rm_eo = (regoff_t)len,
    };
    int rc = regexec(regex, (const char *)buf, 1, &match, REG_STARTEND);
    if (rc != 0)
        return -1;
    if (match.rm_so < 0 || match.rm_eo < 0)
        return -1;
    out->start = (size_t)match.rm_so;
    out->end = (size_t)match.rm_eo;
    return 0;
#else
    size_t chunk_start = start;

    while (chunk_start <= len) {
        const unsigned char *chunk_end = memchr(buf + chunk_start, '\0', len - chunk_start);
        size_t chunk_len = chunk_end ? (size_t)(chunk_end - (buf + chunk_start))
                                     : (len - chunk_start);
        char *chunk = malloc(chunk_len + 1u);
        if (!chunk)
            return -1;
        memcpy(chunk, buf + chunk_start, chunk_len);
        chunk[chunk_len] = '\0';

        regmatch_t match = {0};
        int eflags = 0;
        if (chunk_start > 0u)
            eflags |= REG_NOTBOL;
        if (chunk_end)
            eflags |= REG_NOTEOL;

        int rc = regexec(regex, chunk, 1, &match, eflags);
        free(chunk);
        if (rc == 0) {
            if (match.rm_so < 0 || match.rm_eo < 0)
                return -1;
            out->start = chunk_start + (size_t)match.rm_so;
            out->end = chunk_start + (size_t)match.rm_eo;
            return 0;
        }

        if (!chunk_end)
            break;
        chunk_start += chunk_len + 1u;
    }

    return -1;
#endif
}

static int matcher_find(struct bx_matcher *m,
                        const unsigned char *buf,
                        size_t len,
                        size_t start,
                        struct bx_match *out) {
    if (m->kind == MATCHER_LITERAL)
        return bx_literal_find(m->literal, buf, len, start, out);
    if (m->kind == MATCHER_LITERAL_SET) {
        struct bx_match best = {0};
        bool found = false;

        for (size_t i = 0; i < m->literal_set.count; ++i) {
            struct bx_match candidate = {0};

            if (bx_literal_find(m->literal_set.items[i], buf, len, start, &candidate) != 0)
                continue;
            if (!found || candidate.start < best.start ||
                (candidate.start == best.start && candidate.end < best.end)) {
                best = candidate;
                found = true;
            }
        }

        if (!found)
            return -1;
        *out = best;
        return 0;
    }
    if (m->kind == MATCHER_POSIX) {
        if (start > len)
            return -1;
        return matcher_find_posix_portable(&m->posix, buf, len, start, out);
    }

    return bx_regex_find(m->regex, buf, len, start, out);
}

static bool match_has_word_boundaries(const unsigned char *buf,
                                      size_t len,
                                      const struct bx_match *match,
                                      const struct search_opts *opts) {
    if (opts && opts->locale_word_regexp && bx_rg_locale_is_utf8()) {
        return bx_rg_match_has_locale_word_boundaries_utf8(buf, len,
                                                           match->start,
                                                           match->end);
    }
    return bx_rg_match_has_word_boundaries(buf, len, match->start, match->end,
                                           opts && opts->unicode);
}

int bx_search_matcher_find_with_opts(struct bx_matcher *m,
                                     const unsigned char *buf,
                                     size_t len,
                                     size_t start,
                                     struct search_opts *opts,
                                     struct bx_match *out) {
    bx_search_dev_counters_note_matcher_invocation();
    if (opts->line_regexp &&
        (m->kind == MATCHER_LITERAL || m->kind == MATCHER_LITERAL_SET)) {
        if (start != 0u)
            return -1;
        if (m->kind == MATCHER_LITERAL) {
            if (!bx_literal_verify_at(m->literal, buf, len, 0u, out))
                return -1;
            return out->start == 0u && out->end == len ? 0 : -1;
        }

        for (size_t i = 0; i < m->literal_set.count; ++i) {
            if (!bx_literal_verify_at(m->literal_set.items[i], buf, len, 0u, out))
                continue;
            if (out->start == 0u && out->end == len)
                return 0;
        }
        return -1;
    }

    size_t pos = start;
    while (pos <= len) {
        if (matcher_find(m, buf, len, pos, out) != 0)
            return -1;
        if (!opts->word_regexp || match_has_word_boundaries(buf, len, out, opts))
            return 0;
        pos = out->end > out->start ? out->start + 1u : out->start + 1u;
    }
    return -1;
}

bool bx_search_matcher_verify_literal_candidate_with_opts(struct bx_matcher *m,
                                                          const unsigned char *buf,
                                                          size_t len,
                                                          size_t candidate_start,
                                                          struct search_opts *opts,
                                                          struct bx_match *out) {
    if (!m || m->kind != MATCHER_LITERAL || !opts)
        return false;
    bx_search_dev_counters_note_matcher_invocation();
    if (opts->line_regexp) {
        if (candidate_start != 0u)
            return false;
        if (!bx_literal_verify_at(m->literal, buf, len, 0u, out))
            return false;
        return out->start == 0u && out->end == len;
    }

    if (!bx_literal_verify_at(m->literal, buf, len, candidate_start, out))
        return false;
    if (opts->word_regexp && !match_has_word_boundaries(buf, len, out, opts))
        return false;
    return true;
}

static void matcher_free(struct bx_matcher *m) {
    if (!m)
        return;
    if (m->kind == MATCHER_LITERAL)
        bx_literal_free(m->literal);
    else if (m->kind == MATCHER_LITERAL_SET) {
        for (size_t i = 0; i < m->literal_set.count; ++i)
            bx_literal_free(m->literal_set.items[i]);
        free(m->literal_set.items);
    } else if (m->kind == MATCHER_POSIX) {
        regfree(&m->posix);
    } else {
        bx_regex_free(m->regex);
    }
    free(m);
}

void bx_search_matcher_free(struct bx_matcher *m) {
    matcher_free(m);
}

struct bx_literal_matcher *bx_search_matcher_literal(const struct bx_matcher *m) {
    if (!m || m->kind != MATCHER_LITERAL)
        return NULL;
    return m->literal;
}

static struct bx_matcher *compile_matcher(const char *pattern,
                                          enum bx_search_personality personality,
                                          struct search_opts *opts,
                                          char **errmsg,
                                          char **warningmsg) {
    char *bre_rewritten = NULL;
    char *wrapped = NULL;
    char *collation_rewritten = NULL;
    const char *base_pattern = pattern;
    const char *final_pattern = pattern;
    int flags = 0;
    bool use_posix = matcher_uses_posix(pattern, personality, opts);
    bool locale_utf8_icase = false;
    bool use_literal = false;
    bool ere_has_backrefs = false;
    bool ere_invalid_backref = false;
    size_t literal_pattern_count = 1u;

    if (warningmsg)
        *warningmsg = NULL;
    if (opts->num_extra_patterns > 0 && personality != BX_SEARCH_RG &&
        opts->perl_regexp) {
        if (errmsg && !*errmsg)
            *errmsg = strdup("the -P option only supports a single pattern");
        return NULL;
    }

    if (opts->ignore_case)
        flags |= BX_REGEX_ICASE;

    if (opts->multiline)
        flags |= BX_REGEX_MULTILINE;
    if (opts->multiline_dotall)
        flags |= BX_REGEX_DOTALL;
    locale_utf8_icase = (flags & BX_REGEX_ICASE) != 0 &&
                        personality != BX_SEARCH_RG &&
                        bx_rg_locale_is_utf8();
    use_literal = opts->fixed_strings || pattern_is_plain_literal(base_pattern);
    if (search_is_gnu_bre_mode(personality, opts) == false &&
        opts != NULL &&
        personality != BX_SEARCH_RG &&
        opts->extended_regex) {
        search_pattern_ere_backrefs(base_pattern, &ere_has_backrefs, &ere_invalid_backref);
        if (ere_invalid_backref) {
            if (errmsg && !*errmsg)
                *errmsg = strdup("Invalid back reference");
            return NULL;
        }
        if (ere_has_backrefs)
            use_posix = false;
    }

    if (opts->line_regexp && !use_literal) {
        size_t plen = strlen(final_pattern);
        wrapped = malloc(plen + 3u);
        if (!wrapped)
            return NULL;

        char *p = wrapped;
        *p++ = '^';
        memcpy(p, final_pattern, plen);
        p += plen;
        *p++ = '$';
        *p = '\0';
        final_pattern = wrapped;
    }

    if (search_is_gnu_bre_mode(personality, opts)) {
        bre_rewritten = search_rewrite_gnu_bre_escapes(final_pattern, warningmsg, errmsg);
        if (!bre_rewritten) {
            free(wrapped);
            return NULL;
        }
        final_pattern = bre_rewritten;
    }

    if (personality != BX_SEARCH_RG && !opts->fixed_strings) {
        collation_rewritten = search_rewrite_simple_collation_tokens(final_pattern, errmsg);
        if (!collation_rewritten) {
            if (errmsg && *errmsg) {
                free(bre_rewritten);
                free(wrapped);
                return NULL;
            }
            free(bre_rewritten);
            free(wrapped);
            return NULL;
        }
        final_pattern = collation_rewritten;
    }

    if (opts->smart_case && !opts->ignore_case) {
        bool has_upper = false;
        for (const char *c = final_pattern; *c; c++) {
            if (*c >= 'A' && *c <= 'Z') {
                has_upper = true;
                break;
            }
        }
        if (!has_upper)
            flags |= BX_REGEX_ICASE;
    }

    struct bx_matcher *m = calloc(1u, sizeof(*m));
    if (!m) {
        free(collation_rewritten);
        free(bre_rewritten);
        free(wrapped);
        return NULL;
    }

    if (opts->fixed_strings) {
        literal_pattern_count = 1u + (size_t)opts->num_extra_patterns;
        if (literal_pattern_count > 1u) {
            m->literal_set.items = calloc(literal_pattern_count,
                                          sizeof(*m->literal_set.items));
            if (!m->literal_set.items) {
                free(collation_rewritten);
                free(bre_rewritten);
                free(wrapped);
                free(m);
                return NULL;
            }

            if (bx_literal_compile(&m->literal_set.items[0], final_pattern,
                                   (flags & BX_REGEX_ICASE) != 0,
                                   locale_utf8_icase) != 0) {
                if (errmsg && !*errmsg)
                    *errmsg = strdup("empty fixed-string pattern is not supported");
                matcher_free(m);
                free(collation_rewritten);
                free(bre_rewritten);
                free(wrapped);
                return NULL;
            }
            m->literal_set.count = 1u;

            for (int k = 0; k < opts->num_extra_patterns; ++k) {
                if (bx_literal_compile(&m->literal_set.items[m->literal_set.count],
                                       opts->extra_patterns[k],
                                       (flags & BX_REGEX_ICASE) != 0,
                                       locale_utf8_icase) != 0) {
                    if (errmsg && !*errmsg)
                        *errmsg = strdup("empty fixed-string pattern is not supported");
                    matcher_free(m);
                    free(collation_rewritten);
                    free(bre_rewritten);
                    free(wrapped);
                    return NULL;
                }
                m->literal_set.count++;
            }

            m->kind = MATCHER_LITERAL_SET;
            free(collation_rewritten);
            free(bre_rewritten);
            free(wrapped);
            return m;
        }
    }

    if (use_literal) {
        if (bx_literal_compile(&m->literal, base_pattern, (flags & BX_REGEX_ICASE) != 0,
                               locale_utf8_icase) != 0) {
            if (errmsg && !*errmsg)
                *errmsg = strdup("empty fixed-string pattern is not supported");
            free(collation_rewritten);
            free(bre_rewritten);
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_LITERAL;
    } else if (use_posix) {
        int cflags = 0;
        if (personality == BX_SEARCH_RG || opts->extended_regex)
            cflags |= REG_EXTENDED;
        if (personality != BX_SEARCH_RG)
            cflags |= REG_NEWLINE;
        if (flags & BX_REGEX_ICASE)
            cflags |= REG_ICASE;

        int rc = regcomp(&m->posix, final_pattern, cflags);
        if (rc != 0) {
            if (errmsg && !*errmsg)
                *errmsg = bx_regex_strerror_dup(rc, &m->posix);
            free(collation_rewritten);
            free(bre_rewritten);
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_POSIX;
    } else {
        if (bx_regex_compile(&m->regex, final_pattern, flags, errmsg) != 0) {
            free(collation_rewritten);
            free(bre_rewritten);
            free(wrapped);
            free(m);
            return NULL;
        }
        m->kind = MATCHER_REGEX;
    }

    free(collation_rewritten);
    free(bre_rewritten);
    free(wrapped);
    return m;
}

struct bx_matcher *bx_search_compile_matcher(const char *pattern,
                                             enum bx_search_personality personality,
                                             struct search_opts *opts,
                                             char **errmsg,
                                             char **warningmsg) {
    return compile_matcher(pattern, personality, opts, errmsg, warningmsg);
}

bool bx_search_matcher_is_scanner_literal_eligible(const struct bx_matcher *m,
                                                   const struct search_opts *opts) {
    if (!m || !opts || m->kind != MATCHER_LITERAL)
        return false;
    return !bx_literal_contains_byte(m->literal,
                                     (unsigned char)bx_search_record_delimiter(opts));
}

int bx_search_count_record_matches(struct bx_matcher *m,
                                   const unsigned char *buf,
                                   size_t len,
                                   struct search_opts *opts) {
    size_t start = 0u;
    int count = 0;

    while (start <= len) {
        struct bx_match bm;

        if (bx_search_matcher_find_with_opts(m, buf, len, start, opts, &bm) != 0)
            break;
        count++;
        if (bm.end > bm.start)
            start = bm.end;
        else
            start = bm.start + 1u;
    }
    return count;
}
