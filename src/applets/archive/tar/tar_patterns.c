#include <fnmatch.h>
#include <stdbool.h>
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "applets/archive/tar/tar_patterns.h"
#include "bx/libbx.h"
#include "lib/glob_syntax.h"

static bool bx_tar_match_char_equal(unsigned char left,
                                    unsigned char right,
                                    bool ignore_case) {
    if (ignore_case) {
        left = (unsigned char)tolower(left);
        right = (unsigned char)tolower(right);
    }
    return left == right;
}

static size_t bx_tar_match_trimmed_len(const char* text) {
    size_t len = strlen(text);

    while (len > 0u && text[len - 1u] == '/') {
        len--;
    }
    return len;
}

static bool bx_tar_match_text_equal_len(const char* left,
                                        size_t left_len,
                                        const char* right,
                                        size_t right_len,
                                        bool ignore_case) {
    size_t i;

    if (left_len != right_len) {
        return false;
    }
    for (i = 0u; i < left_len; i++) {
        if (!bx_tar_match_char_equal((unsigned char)left[i],
                                     (unsigned char)right[i],
                                     ignore_case)) {
            return false;
        }
    }
    return true;
}

static bool bx_tar_match_pattern_has_wildcard_magic(const char* text) {
    size_t i;

    for (i = 0u; text[i] != '\0'; i++) {
        switch (text[i]) {
            case '*':
            case '?':
            case '[':
            case '\\':
                return true;
        }
    }
    return false;
}

static bool bx_tar_match_pattern_has_slash(const char* text) {
    return strchr(text, '/') != NULL;
}

static char* bx_tar_match_dup_folded_range(const char* text,
                                           size_t len,
                                           bool ignore_case,
                                           size_t spare) {
    char* out = xmalloc(len + 1u + spare);
    size_t i;

    for (i = 0u; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        out[i] = ignore_case ? (char)tolower(ch) : (char)ch;
    }
    out[len] = '\0';
    return out;
}

static bool bx_tar_match_wildcard_text(const struct bx_tar_match_pattern* pattern,
                                       const char* text, size_t text_len) {
    int flags = pattern->policy.wildcards_match_slash ? 0 : FNM_PATHNAME;
    const char* wildcard = pattern->wildcard_text ? pattern->wildcard_text : pattern->text;
    if (!pattern->policy.ignore_case && text[text_len] == '\0') {
        return fnmatch(wildcard, text, flags) == 0;
    }
    char* text_cmp = bx_tar_match_dup_folded_range(text, text_len, pattern->policy.ignore_case, 0);
    bool matched = fnmatch(wildcard, text_cmp, flags) == 0;
    free(text_cmp);
    return matched;
}

static bool bx_tar_match_literal_text_len(const char* pattern,
                                          size_t pattern_len,
                                          const struct bx_tar_match_policy* policy,
                                          bool recurse,
                                          const char* text,
                                          size_t text_len) {
    if (bx_tar_match_text_equal_len(pattern,
                                    pattern_len,
                                    text,
                                    text_len,
                                    policy->ignore_case)) {
        return true;
    }
    if (!recurse || text_len <= pattern_len) {
        return false;
    }
    return text[pattern_len] == '/'
        && bx_tar_match_text_equal_len(pattern,
                                       pattern_len,
                                       text,
                                       pattern_len,
                                       policy->ignore_case);
}

static bool bx_tar_match_pattern_text(const struct bx_tar_match_pattern* pattern,
                                      bool recurse,
                                      const char* text,
                                      size_t text_len) {
    if (pattern->policy.wildcards && pattern->wildcard_magic) {
        if (bx_tar_match_wildcard_text(pattern, text, text_len)) {
            return true;
        }
        if (!recurse) {
            return false;
        }
        while (text_len > 0u) {
            text_len--;
            if (text[text_len] == '/'
                && bx_tar_match_wildcard_text(pattern, text, text_len)) {
                return true;
            }
        }
        return false;
    }

    return bx_tar_match_literal_text_len(pattern->text,
                                         pattern->trimmed_len,
                                         &pattern->policy,
                                         recurse,
                                         text,
                                         text_len);
}

static bool bx_tar_match_scan_name_pattern(const struct bx_tar_match_pattern* pattern,
                                           bool recurse,
                                           const char* name,
                                           bool trim_name) {
    size_t name_len = trim_name ? bx_tar_match_trimmed_len(name) : strlen(name);
    const char* name_end = name + name_len;
    const char* cursor = name;

    if (recurse
        && !pattern->policy.anchored
        && !pattern->wildcard_magic
        && !pattern->has_slash) {
        const char* segment = name;
        const char* p = name;

        while (true) {
            if (p == name_end || *p == '/') {
                if (bx_tar_match_text_equal_len(pattern->text,
                                                pattern->trimmed_len,
                                                segment,
                                                (size_t)(p - segment),
                                                pattern->policy.ignore_case)) {
                    return true;
                }
                if (p == name_end) {
                    break;
                }
                segment = p + 1;
            }
            p++;
        }
        return false;
    }

    if (bx_tar_match_pattern_text(pattern, recurse, cursor, name_len)) {
        return true;
    }

    if (pattern->policy.anchored) {
        return false;
    }

    while (cursor < name_end) {
        if (*cursor == '/'
            && bx_tar_match_pattern_text(pattern,
                                         recurse,
                                         cursor + 1,
                                         name_len - (size_t)((cursor + 1) - name))) {
            return true;
        }
        cursor++;
    }

    return false;
}

void bx_tar_match_policy_init_member_default(struct bx_tar_match_policy* policy) {
    policy->anchored = true;
    policy->ignore_case = false;
    policy->wildcards = false;
    policy->wildcards_match_slash = false;
}

void bx_tar_match_policy_init_exclude_default(struct bx_tar_match_policy* policy) {
    policy->anchored = false;
    policy->ignore_case = false;
    policy->wildcards = true;
    policy->wildcards_match_slash = false;
}

bool bx_tar_match_policy_set_anchored(struct bx_tar_match_policy* member_policy,
                                      struct bx_tar_match_policy* exclude_policy,
                                      bool enabled) {
    member_policy->anchored = enabled;
    exclude_policy->anchored = enabled;
    return true;
}

bool bx_tar_match_policy_set_ignore_case(struct bx_tar_match_policy* member_policy,
                                         struct bx_tar_match_policy* exclude_policy,
                                         bool enabled) {
    member_policy->ignore_case = enabled;
    exclude_policy->ignore_case = enabled;
    return true;
}

bool bx_tar_match_policy_set_wildcards(struct bx_tar_match_policy* member_policy,
                                       struct bx_tar_match_policy* exclude_policy,
                                       bool enabled) {
    member_policy->wildcards = enabled;
    exclude_policy->wildcards = enabled;
    return true;
}

bool bx_tar_match_policy_set_wildcards_match_slash(struct bx_tar_match_policy* member_policy,
                                                   struct bx_tar_match_policy* exclude_policy,
                                                   bool enabled) {
    member_policy->wildcards_match_slash = enabled;
    exclude_policy->wildcards_match_slash = enabled;
    return true;
}

void bx_tar_match_pattern_init(struct bx_tar_match_pattern* pattern, const char* text,
                              const struct bx_tar_match_policy* policy) {
    *pattern = (struct bx_tar_match_pattern){
        .text = xstrdup(text),
        .trimmed_len = bx_tar_match_trimmed_len(text),
        .wildcard_magic = bx_tar_match_pattern_has_wildcard_magic(text),
        .has_slash = bx_tar_match_pattern_has_slash(text),
        .policy = *policy,
    };
    bool normalize = policy->wildcards && strstr(text, "[^") != NULL;
    if (policy->wildcards && pattern->wildcard_magic && (normalize || policy->ignore_case)) {
        pattern->wildcard_text = bx_tar_match_dup_folded_range(text, strlen(text), policy->ignore_case,
                                                              normalize ? BX_GLOB_CARET_SPARE : 0);
        if (normalize)
            bx_glob_normalize_caret(pattern->wildcard_text);
    }
}

void bx_tar_match_pattern_free(struct bx_tar_match_pattern* pattern) {
    free(pattern->text);
    free(pattern->wildcard_text);
}

void bx_tar_match_pattern_list_free(struct bx_tar_match_pattern_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        bx_tar_match_pattern_free(&list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

bool bx_tar_match_pattern_list_append(struct bx_tar_match_pattern_list* list,
                                      const char* text,
                                      const struct bx_tar_match_policy* policy) {
    struct bx_tar_match_pattern* slot;

    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 8u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }

    slot = &list->items[list->len++];
    bx_tar_match_pattern_init(slot, text, policy);
    return true;
}

bool bx_tar_match_member_name(const struct bx_tar_match_pattern* pattern,
                              bool recurse, const char* name) {
    return bx_tar_match_scan_name_pattern(pattern, recurse, name, true);
}

bool bx_tar_path_excluded(const struct bx_tar_match_pattern_list* patterns,
                          const char* archive_path) {
    size_t i;

    for (i = 0u; i < patterns->len; i++) {
        if (bx_tar_match_scan_name_pattern(&patterns->items[i], true, archive_path, false)) {
            return true;
        }
    }
    return false;
}
