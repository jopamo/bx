#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dev_counters.h"
#include "ignore_program.h"

struct bx_ignore_rule {
    char *pattern;
    bool negate;
    bool uses_relative_path;
    bool directory_only;
};

struct bx_ignore_literal_basename {
    const char *name;
    int rule_index;
    enum bx_ignore_match_result result;
};

struct bx_ignore_literal_extension {
    const char *suffix;
    int rule_index;
    enum bx_ignore_match_result result;
};

struct bx_ignore_literal_directory {
    const char *name;
    int rule_index;
    enum bx_ignore_match_result result;
};

struct bx_ignore_anchored_prefix {
    const char *prefix;
    size_t prefix_len;
    int rule_index;
    enum bx_ignore_match_result result;
    bool directory_only;
};

struct bx_ignore_program {
    atomic_uint refcount;
    struct bx_ignore_rule *rules;
    int rule_count;
    struct bx_ignore_literal_basename *literal_basenames;
    int literal_basename_count;
    struct bx_ignore_literal_extension *literal_extensions;
    int literal_extension_count;
    struct bx_ignore_literal_directory *literal_directories;
    int literal_directory_count;
    struct bx_ignore_anchored_prefix *anchored_prefixes;
    int anchored_prefix_count;
    bool casefold;
};

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_extension(const struct bx_ignore_program *program,
                                                const char *name,
                                                const char *relative_path,
                                                bool is_dir);
static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_directory(const struct bx_ignore_program *program,
                                                const char *name,
                                                const char *relative_path,
                                                bool is_dir);
static enum bx_ignore_match_result
bx_ignore_program_match_after_anchored_prefix(const struct bx_ignore_program *program,
                                              const char *name,
                                              const char *relative_path,
                                              bool is_dir);
static enum bx_ignore_match_result
bx_ignore_program_match_generic_rules_from(const struct bx_ignore_program *program,
                                           const char *name,
                                           const char *relative_path,
                                           bool is_dir,
                                           int start_index,
                                           int stop_exclusive);

static void bx_ignore_rule_dispose(struct bx_ignore_rule *rule) {
    if (!rule)
        return;
    free(rule->pattern);
    rule->pattern = NULL;
    rule->negate = false;
    rule->uses_relative_path = false;
    rule->directory_only = false;
}

static bool bx_ignore_rule_compile(struct bx_ignore_rule *rule, const char *line) {
    const char *pattern = line;

    if (!rule || !line)
        return false;

    while (*pattern == ' ')
        pattern++;
    if (*pattern == '#' || *pattern == '\0')
        return false;

    rule->negate = false;
    if (*pattern == '!') {
        rule->negate = true;
        pattern++;
    }
    if (*pattern == '/')
        pattern++;
    if (*pattern == '\0')
        return false;

    size_t pattern_len = strlen(pattern);
    if (pattern_len > 0u && pattern[pattern_len - 1u] == '/') {
        rule->directory_only = true;
        pattern_len--;
    } else {
        rule->directory_only = false;
    }
    if (pattern_len == 0u)
        return false;

    rule->uses_relative_path = memchr(pattern, '/', pattern_len) != NULL;
    rule->pattern = strndup(pattern, pattern_len);
    return rule->pattern != NULL;
}

static bool bx_ignore_rule_is_literal_basename(const struct bx_ignore_rule *rule) {
    if (!rule || !rule->pattern || rule->pattern[0] == '\0' || rule->directory_only)
        return false;
    return strpbrk(rule->pattern, "*?[\\") == NULL && strchr(rule->pattern, '/') == NULL;
}

static bool bx_ignore_rule_is_literal_extension(const struct bx_ignore_rule *rule) {
    if (!rule || !rule->pattern || rule->directory_only)
        return false;
    if (rule->pattern[0] != '*' || rule->pattern[1] != '.' || rule->pattern[2] == '\0')
        return false;
    return strpbrk(rule->pattern + 1, "*?[\\") == NULL && strchr(rule->pattern + 1, '/') == NULL;
}

static bool bx_ignore_rule_is_literal_directory(const struct bx_ignore_rule *rule) {
    if (!rule || !rule->pattern || rule->pattern[0] == '\0' || !rule->directory_only)
        return false;
    return strpbrk(rule->pattern, "*?[\\") == NULL && strchr(rule->pattern, '/') == NULL;
}

static bool bx_ignore_rule_is_anchored_prefix_candidate(const struct bx_ignore_rule *rule) {
    if (!rule || !rule->pattern || rule->pattern[0] == '\0')
        return false;
    if (!rule->directory_only && !rule->uses_relative_path)
        return false;
    return strpbrk(rule->pattern, "*?[\\") == NULL;
}

static bool bx_ignore_rule_uses_generic_glob_fallback(const struct bx_ignore_rule *rule) {
    if (!rule || !rule->pattern || rule->pattern[0] == '\0')
        return false;
    return !bx_ignore_rule_is_literal_basename(rule) &&
           !bx_ignore_rule_is_literal_extension(rule) &&
           !bx_ignore_rule_is_literal_directory(rule) &&
           !bx_ignore_rule_is_anchored_prefix_candidate(rule);
}

static int bx_ignore_literal_basename_cmp_case_sensitive(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_basename *a = lhs;
    const struct bx_ignore_literal_basename *b = rhs;
    int cmp = strcmp(a->name, b->name);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static int bx_ignore_literal_basename_cmp_casefold(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_basename *a = lhs;
    const struct bx_ignore_literal_basename *b = rhs;
    int cmp = strcasecmp(a->name, b->name);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static int bx_ignore_literal_extension_cmp_case_sensitive(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_extension *a = lhs;
    const struct bx_ignore_literal_extension *b = rhs;
    int cmp = strcmp(a->suffix, b->suffix);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static int bx_ignore_literal_extension_cmp_casefold(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_extension *a = lhs;
    const struct bx_ignore_literal_extension *b = rhs;
    int cmp = strcasecmp(a->suffix, b->suffix);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static int bx_ignore_literal_directory_cmp_case_sensitive(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_directory *a = lhs;
    const struct bx_ignore_literal_directory *b = rhs;
    int cmp = strcmp(a->name, b->name);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static int bx_ignore_literal_directory_cmp_casefold(const void *lhs, const void *rhs) {
    const struct bx_ignore_literal_directory *a = lhs;
    const struct bx_ignore_literal_directory *b = rhs;
    int cmp = strcasecmp(a->name, b->name);
    if (cmp != 0)
        return cmp;
    if (a->rule_index < b->rule_index)
        return -1;
    if (a->rule_index > b->rule_index)
        return 1;
    return 0;
}

static const struct bx_ignore_literal_basename *
bx_ignore_program_find_literal_basename(const struct bx_ignore_program *program,
                                        const char *name) {
    if (!program || !name || !program->literal_basenames || program->literal_basename_count <= 0)
        return NULL;

    int lo = 0;
    int hi = program->literal_basename_count;
    while (lo < hi) {
        int mid = lo + ((hi - lo) / 2);
        const struct bx_ignore_literal_basename *entry = &program->literal_basenames[mid];
        int cmp = program->casefold ? strcasecmp(name, entry->name) : strcmp(name, entry->name);

        if (cmp == 0)
            return entry;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static const struct bx_ignore_literal_extension *
bx_ignore_program_find_literal_extension_exact(const struct bx_ignore_program *program,
                                               const char *suffix) {
    if (!program || !suffix || !program->literal_extensions || program->literal_extension_count <= 0)
        return NULL;

    int lo = 0;
    int hi = program->literal_extension_count;
    while (lo < hi) {
        int mid = lo + ((hi - lo) / 2);
        const struct bx_ignore_literal_extension *entry = &program->literal_extensions[mid];
        int cmp = program->casefold ? strcasecmp(suffix, entry->suffix)
                                    : strcmp(suffix, entry->suffix);

        if (cmp == 0)
            return entry;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static const struct bx_ignore_literal_extension *
bx_ignore_program_find_literal_extension(const struct bx_ignore_program *program,
                                         const char *name) {
    const struct bx_ignore_literal_extension *best = NULL;

    if (!program || !name || !program->literal_extensions || program->literal_extension_count <= 0)
        return NULL;

    for (const char *dot = strchr(name, '.'); dot; dot = strchr(dot + 1, '.')) {
        const struct bx_ignore_literal_extension *entry =
            bx_ignore_program_find_literal_extension_exact(program, dot);
        if (!entry)
            continue;
        if (!best || entry->rule_index > best->rule_index)
            best = entry;
    }
    return best;
}

static const struct bx_ignore_literal_directory *
bx_ignore_program_find_literal_directory(const struct bx_ignore_program *program,
                                         const char *name) {
    if (!program || !name || !program->literal_directories || program->literal_directory_count <= 0)
        return NULL;

    int lo = 0;
    int hi = program->literal_directory_count;
    while (lo < hi) {
        int mid = lo + ((hi - lo) / 2);
        const struct bx_ignore_literal_directory *entry = &program->literal_directories[mid];
        int cmp = program->casefold ? strcasecmp(name, entry->name) : strcmp(name, entry->name);

        if (cmp == 0)
            return entry;
        if (cmp < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

static bool bx_ignore_program_prefix_equal(const struct bx_ignore_program *program,
                                           const char *lhs,
                                           const char *rhs,
                                           size_t len) {
    if (!lhs || !rhs)
        return false;
    return program->casefold ? strncasecmp(lhs, rhs, len) == 0 : strncmp(lhs, rhs, len) == 0;
}

static bool bx_ignore_rule_matches_anchored_prefix(const struct bx_ignore_program *program,
                                                   const struct bx_ignore_rule *rule,
                                                   const char *relative_path,
                                                   bool is_dir) {
    size_t prefix_len;

    if (!program || !bx_ignore_rule_is_anchored_prefix_candidate(rule) || !relative_path)
        return false;

    prefix_len = strlen(rule->pattern);
    if (!bx_ignore_program_prefix_equal(program, relative_path, rule->pattern, prefix_len))
        return false;
    if (relative_path[prefix_len] == '\0')
        return !rule->directory_only || is_dir;
    return relative_path[prefix_len] == '/';
}

static const struct bx_ignore_anchored_prefix *
bx_ignore_program_find_anchored_prefix(const struct bx_ignore_program *program,
                                       const char *relative_path,
                                       bool is_dir) {
    const struct bx_ignore_anchored_prefix *best = NULL;

    if (!program || !relative_path || !program->anchored_prefixes || program->anchored_prefix_count <= 0)
        return NULL;

    for (int i = 0; i < program->anchored_prefix_count; ++i) {
        const struct bx_ignore_anchored_prefix *entry = &program->anchored_prefixes[i];
        if (!bx_ignore_program_prefix_equal(program, relative_path, entry->prefix, entry->prefix_len))
            continue;
        if (relative_path[entry->prefix_len] == '\0') {
            if (entry->directory_only && !is_dir)
                continue;
        } else if (relative_path[entry->prefix_len] != '/') {
            continue;
        }
        if (!best || entry->rule_index > best->rule_index)
            best = entry;
    }
    return best;
}

static enum bx_ignore_match_result bx_ignore_program_match_rule(const struct bx_ignore_program *program,
                                                                const struct bx_ignore_rule *rule,
                                                                const char *name,
                                                                const char *relative_path,
                                                                bool is_dir) {
    int flags;
    const char *candidate;

    if (!program || !rule)
        return BX_IGNORE_NO_MATCH;

    flags = rule->uses_relative_path ? FNM_PATHNAME : 0;
    candidate = rule->uses_relative_path ? relative_path : name;

    if (bx_ignore_rule_is_anchored_prefix_candidate(rule)) {
        if (bx_ignore_rule_matches_anchored_prefix(program, rule, relative_path, is_dir))
            return rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE;
        return BX_IGNORE_NO_MATCH;
    }

    if (program->casefold)
        flags |= FNM_CASEFOLD;
    if (!candidate || candidate[0] == '\0')
        return BX_IGNORE_NO_MATCH;
    if (rule->directory_only && !is_dir)
        return BX_IGNORE_NO_MATCH;

    /* Literal tables are accelerators only; generic glob rules still fall back to fnmatch(). */
    if (bx_ignore_rule_uses_generic_glob_fallback(rule))
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS, 1u);
    if (fnmatch(rule->pattern, candidate, flags) != 0)
        return BX_IGNORE_NO_MATCH;
    return rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE;
}

static enum bx_ignore_match_result bx_ignore_program_match_rules_from(const struct bx_ignore_program *program,
                                                                      const char *name,
                                                                      const char *relative_path,
                                                                      bool is_dir,
                                                                      int start_index,
                                                                      int stop_exclusive) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    if (start_index >= program->rule_count)
        start_index = program->rule_count - 1;
    for (int i = start_index; i >= 0 && i >= stop_exclusive; --i) {
        enum bx_ignore_match_result result =
            bx_ignore_program_match_rule(program, &program->rules[i], name, relative_path, is_dir);
        if (result != BX_IGNORE_NO_MATCH)
            return result;
    }
    return BX_IGNORE_NO_MATCH;
}

static enum bx_ignore_match_result
bx_ignore_program_match_generic_rules_from(const struct bx_ignore_program *program,
                                           const char *name,
                                           const char *relative_path,
                                           bool is_dir,
                                           int start_index,
                                           int stop_exclusive) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    if (start_index >= program->rule_count)
        start_index = program->rule_count - 1;
    for (int i = start_index; i >= 0 && i >= stop_exclusive; --i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (!bx_ignore_rule_uses_generic_glob_fallback(rule))
            continue;
        enum bx_ignore_match_result result =
            bx_ignore_program_match_rule(program, rule, name, relative_path, is_dir);
        if (result != BX_IGNORE_NO_MATCH)
            return result;
    }
    return BX_IGNORE_NO_MATCH;
}

static bool bx_ignore_program_build_literal_basename_table(struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return true;

    int candidate_count = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        if (bx_ignore_rule_is_literal_basename(&program->rules[i]))
            candidate_count++;
    }
    if (candidate_count == 0)
        return true;

    struct bx_ignore_literal_basename *candidates =
        calloc((size_t)candidate_count, sizeof(*candidates));
    if (!candidates)
        return false;

    int write = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (!bx_ignore_rule_is_literal_basename(rule))
            continue;
        candidates[write++] = (struct bx_ignore_literal_basename){
            .name = rule->pattern,
            .rule_index = i,
            .result = rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE,
        };
    }

    qsort(candidates,
          (size_t)candidate_count,
          sizeof(*candidates),
          program->casefold
              ? bx_ignore_literal_basename_cmp_casefold
              : bx_ignore_literal_basename_cmp_case_sensitive);

    program->literal_basenames = calloc((size_t)candidate_count, sizeof(*program->literal_basenames));
    if (!program->literal_basenames) {
        free(candidates);
        return false;
    }

    for (int i = 0; i < candidate_count; ++i) {
        if (program->literal_basename_count > 0) {
            struct bx_ignore_literal_basename *prev =
                &program->literal_basenames[program->literal_basename_count - 1];
            int cmp = program->casefold ? strcasecmp(prev->name, candidates[i].name)
                                        : strcmp(prev->name, candidates[i].name);
            if (cmp == 0) {
                *prev = candidates[i];
                continue;
            }
        }
        program->literal_basenames[program->literal_basename_count++] = candidates[i];
    }

    free(candidates);
    return true;
}

static bool bx_ignore_program_build_anchored_prefix_table(struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return true;

    int candidate_count = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        if (bx_ignore_rule_is_anchored_prefix_candidate(&program->rules[i]))
            candidate_count++;
    }
    if (candidate_count == 0)
        return true;

    program->anchored_prefixes =
        calloc((size_t)candidate_count, sizeof(*program->anchored_prefixes));
    if (!program->anchored_prefixes)
        return false;

    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (!bx_ignore_rule_is_anchored_prefix_candidate(rule))
            continue;
        program->anchored_prefixes[program->anchored_prefix_count++] =
            (struct bx_ignore_anchored_prefix){
                .prefix = rule->pattern,
                .prefix_len = strlen(rule->pattern),
                .rule_index = i,
                .result = rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE,
                .directory_only = rule->directory_only,
            };
    }
    return true;
}

static bool bx_ignore_program_build_literal_extension_table(struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return true;

    int candidate_count = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        if (bx_ignore_rule_is_literal_extension(&program->rules[i]))
            candidate_count++;
    }
    if (candidate_count == 0)
        return true;

    struct bx_ignore_literal_extension *candidates =
        calloc((size_t)candidate_count, sizeof(*candidates));
    if (!candidates)
        return false;

    int write = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (!bx_ignore_rule_is_literal_extension(rule))
            continue;
        candidates[write++] = (struct bx_ignore_literal_extension){
            .suffix = rule->pattern + 1,
            .rule_index = i,
            .result = rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE,
        };
    }

    qsort(candidates,
          (size_t)candidate_count,
          sizeof(*candidates),
          program->casefold
              ? bx_ignore_literal_extension_cmp_casefold
              : bx_ignore_literal_extension_cmp_case_sensitive);

    program->literal_extensions = calloc((size_t)candidate_count, sizeof(*program->literal_extensions));
    if (!program->literal_extensions) {
        free(candidates);
        return false;
    }

    for (int i = 0; i < candidate_count; ++i) {
        if (program->literal_extension_count > 0) {
            struct bx_ignore_literal_extension *prev =
                &program->literal_extensions[program->literal_extension_count - 1];
            int cmp = program->casefold ? strcasecmp(prev->suffix, candidates[i].suffix)
                                        : strcmp(prev->suffix, candidates[i].suffix);
            if (cmp == 0) {
                *prev = candidates[i];
                continue;
            }
        }
        program->literal_extensions[program->literal_extension_count++] = candidates[i];
    }

    free(candidates);
    return true;
}

static bool bx_ignore_program_build_literal_directory_table(struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return true;

    int candidate_count = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        if (bx_ignore_rule_is_literal_directory(&program->rules[i]))
            candidate_count++;
    }
    if (candidate_count == 0)
        return true;

    struct bx_ignore_literal_directory *candidates =
        calloc((size_t)candidate_count, sizeof(*candidates));
    if (!candidates)
        return false;

    int write = 0;
    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (!bx_ignore_rule_is_literal_directory(rule))
            continue;
        candidates[write++] = (struct bx_ignore_literal_directory){
            .name = rule->pattern,
            .rule_index = i,
            .result = rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE,
        };
    }

    qsort(candidates,
          (size_t)candidate_count,
          sizeof(*candidates),
          program->casefold
              ? bx_ignore_literal_directory_cmp_casefold
              : bx_ignore_literal_directory_cmp_case_sensitive);

    program->literal_directories = calloc((size_t)candidate_count, sizeof(*program->literal_directories));
    if (!program->literal_directories) {
        free(candidates);
        return false;
    }

    for (int i = 0; i < candidate_count; ++i) {
        if (program->literal_directory_count > 0) {
            struct bx_ignore_literal_directory *prev =
                &program->literal_directories[program->literal_directory_count - 1];
            int cmp = program->casefold ? strcasecmp(prev->name, candidates[i].name)
                                        : strcmp(prev->name, candidates[i].name);
            if (cmp == 0) {
                *prev = candidates[i];
                continue;
            }
        }
        program->literal_directories[program->literal_directory_count++] = candidates[i];
    }

    free(candidates);
    return true;
}

struct bx_ignore_program *bx_ignore_program_compile(char *const *patterns,
                                                    int pattern_count,
                                                    bool casefold) {
    if (!patterns || pattern_count <= 0)
        return NULL;

    struct bx_ignore_program *program = calloc(1u, sizeof(*program));
    if (!program)
        return NULL;

    program->rules = calloc((size_t)pattern_count, sizeof(*program->rules));
    if (!program->rules) {
        free(program);
        return NULL;
    }

    for (int i = 0; i < pattern_count; ++i) {
        if (!patterns[i])
            continue;
        if (!bx_ignore_rule_compile(&program->rules[program->rule_count], patterns[i])) {
            if (program->rules[program->rule_count].pattern)
                goto fail;
            continue;
        }
        program->rule_count++;
    }

    if (program->rule_count == 0) {
        free(program->rules);
        free(program);
        return NULL;
    }

    atomic_init(&program->refcount, 1u);
    program->casefold = casefold;
    if (!bx_ignore_program_build_literal_basename_table(program))
        goto fail;
    if (!bx_ignore_program_build_literal_extension_table(program))
        goto fail;
    if (!bx_ignore_program_build_literal_directory_table(program))
        goto fail;
    if (!bx_ignore_program_build_anchored_prefix_table(program))
        goto fail;
    return program;

fail:
    for (int i = 0; i < program->rule_count; ++i)
        bx_ignore_rule_dispose(&program->rules[i]);
    free(program->anchored_prefixes);
    free(program->literal_directories);
    free(program->literal_extensions);
    free(program->literal_basenames);
    free(program->rules);
    free(program);
    return NULL;
}

struct bx_ignore_program *bx_ignore_program_retain(struct bx_ignore_program *program) {
    if (!program)
        return NULL;
    atomic_fetch_add_explicit(&program->refcount, 1u, memory_order_relaxed);
    return program;
}

void bx_ignore_program_release(struct bx_ignore_program *program) {
    if (!program)
        return;
    if (atomic_fetch_sub_explicit(&program->refcount, 1u, memory_order_acq_rel) != 1u)
        return;
    for (int i = 0; i < program->rule_count; ++i)
        bx_ignore_rule_dispose(&program->rules[i]);
    free(program->anchored_prefixes);
    free(program->literal_directories);
    free(program->literal_extensions);
    free(program->literal_basenames);
    free(program->rules);
    free(program);
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_basename(const struct bx_ignore_program *program,
                                         const char *name,
                                         const char *relative_path,
                                         bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    const struct bx_ignore_literal_basename *literal_basename =
        bx_ignore_program_find_literal_basename(program, name);
    if (literal_basename) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_rules_from(program,
                                               name,
                                               relative_path,
                                               is_dir,
                                               program->rule_count - 1,
                                               literal_basename->rule_index + 1);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        return literal_basename->result;
    }
    return BX_IGNORE_NO_MATCH;
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_basename(const struct bx_ignore_program *program,
                                               const char *name,
                                               const char *relative_path,
                                               bool is_dir) {
    enum bx_ignore_match_result literal_extension =
        bx_ignore_program_match_literal_extension(program, name, relative_path, is_dir);
    if (literal_extension != BX_IGNORE_NO_MATCH)
        return literal_extension;
    return bx_ignore_program_match_after_literal_extension(program, name, relative_path, is_dir);
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_extension(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    const struct bx_ignore_literal_extension *literal_extension =
        bx_ignore_program_find_literal_extension(program, name);
    if (literal_extension) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_rules_from(program,
                                               name,
                                               relative_path,
                                               is_dir,
                                               program->rule_count - 1,
                                               literal_extension->rule_index + 1);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        return literal_extension->result;
    }
    return BX_IGNORE_NO_MATCH;
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_extension(const struct bx_ignore_program *program,
                                                const char *name,
                                                const char *relative_path,
                                                bool is_dir) {
    enum bx_ignore_match_result literal_directory =
        bx_ignore_program_match_literal_directory(program, name, relative_path, is_dir);
    if (literal_directory != BX_IGNORE_NO_MATCH)
        return literal_directory;
    return bx_ignore_program_match_after_literal_directory(program, name, relative_path, is_dir);
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_directory(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    const struct bx_ignore_literal_directory *literal_directory =
        is_dir ? bx_ignore_program_find_literal_directory(program, name) : NULL;
    if (literal_directory) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_rules_from(program,
                                               name,
                                               relative_path,
                                               is_dir,
                                               program->rule_count - 1,
                                               literal_directory->rule_index + 1);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        return literal_directory->result;
    }
    return BX_IGNORE_NO_MATCH;
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_directory(const struct bx_ignore_program *program,
                                                const char *name,
                                                const char *relative_path,
                                                bool is_dir) {
    enum bx_ignore_match_result anchored_prefix =
        bx_ignore_program_match_anchored_prefix(program, name, relative_path, is_dir);
    if (anchored_prefix != BX_IGNORE_NO_MATCH)
        return anchored_prefix;
    return bx_ignore_program_match_after_anchored_prefix(program, name, relative_path, is_dir);
}

enum bx_ignore_match_result
bx_ignore_program_match_anchored_prefix(const struct bx_ignore_program *program,
                                        const char *name,
                                        const char *relative_path,
                                        bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    const struct bx_ignore_anchored_prefix *anchored_prefix =
        bx_ignore_program_find_anchored_prefix(program, relative_path, is_dir);
    if (anchored_prefix) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_rules_from(program,
                                               name,
                                               relative_path,
                                               is_dir,
                                               program->rule_count - 1,
                                               anchored_prefix->rule_index + 1);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        return anchored_prefix->result;
    }
    return BX_IGNORE_NO_MATCH;
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_anchored_prefix(const struct bx_ignore_program *program,
                                              const char *name,
                                              const char *relative_path,
                                              bool is_dir) {
    return bx_ignore_program_match_generic_glob_fallback(program, name, relative_path, is_dir);
}

enum bx_ignore_match_result
bx_ignore_program_match_generic_glob_fallback(const struct bx_ignore_program *program,
                                              const char *name,
                                              const char *relative_path,
                                              bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;

    /* All literal and anchored-prefix rules were already staged earlier; only generic glob rules remain here. */
    return bx_ignore_program_match_generic_rules_from(program,
                                                      name,
                                                      relative_path,
                                                      is_dir,
                                                      program->rule_count - 1,
                                                      0);
}

enum bx_ignore_match_result bx_ignore_program_match(const struct bx_ignore_program *program,
                                                    const char *name,
                                                    const char *relative_path,
                                                    bool is_dir) {
    enum bx_ignore_match_result literal_basename =
        bx_ignore_program_match_literal_basename(program, name, relative_path, is_dir);
    if (literal_basename != BX_IGNORE_NO_MATCH)
        return literal_basename;
    return bx_ignore_program_match_after_literal_basename(program,
                                                          name,
                                                          relative_path,
                                                          is_dir);
}
