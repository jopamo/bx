#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dev_counters.h"
#include "ignore_program.h"

enum bx_ignore_rule_kind {
    BX_IGNORE_RULE_INVALID = 0,
    BX_IGNORE_RULE_LITERAL_BASENAME,
    BX_IGNORE_RULE_LITERAL_EXTENSION,
    BX_IGNORE_RULE_LITERAL_DIRECTORY,
    BX_IGNORE_RULE_ANCHORED_PREFIX,
    BX_IGNORE_RULE_GENERIC_GLOB,
};

struct bx_ignore_rule {
    char *pattern;
    enum bx_ignore_source_kind source;
    enum bx_ignore_rule_kind kind;
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
    bool basename_only;
    unsigned literal_basename_source_mask;
    unsigned literal_extension_source_mask;
    unsigned literal_directory_source_mask;
    unsigned anchored_prefix_source_mask;
    unsigned generic_glob_source_mask;
    bool casefold;
    bool process_lifetime;
};

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
    rule->source = BX_IGNORE_SOURCE_BUILTIN;
    rule->kind = BX_IGNORE_RULE_INVALID;
    rule->negate = false;
    rule->uses_relative_path = false;
    rule->directory_only = false;
}

static void bx_ignore_program_destroy(struct bx_ignore_program *program) {
    if (!program)
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

static bool bx_ignore_source_kind_is_valid(enum bx_ignore_source_kind source) {
    return source == BX_IGNORE_SOURCE_BUILTIN ||
           source == BX_IGNORE_SOURCE_GITIGNORE ||
           source == BX_IGNORE_SOURCE_DOTIGNORE;
}

static unsigned bx_ignore_source_mask(enum bx_ignore_source_kind source) {
    if (!bx_ignore_source_kind_is_valid(source))
        source = BX_IGNORE_SOURCE_BUILTIN;
    return 1u << (unsigned)source;
}

static void bx_ignore_note_source_checks(unsigned source_mask) {
    if (!bx_search_dev_counters_enabled() || source_mask == 0u)
        return;
    if (source_mask & bx_ignore_source_mask(BX_IGNORE_SOURCE_BUILTIN))
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_BUILTIN_CHECKS, 1u);
    if (source_mask & bx_ignore_source_mask(BX_IGNORE_SOURCE_GITIGNORE))
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GITIGNORE_CHECKS, 1u);
    if (source_mask & bx_ignore_source_mask(BX_IGNORE_SOURCE_DOTIGNORE))
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_DOTIGNORE_CHECKS, 1u);
}

static void bx_ignore_note_source_reject(enum bx_ignore_source_kind source) {
    if (!bx_search_dev_counters_enabled())
        return;
    switch (source) {
    case BX_IGNORE_SOURCE_GITIGNORE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GITIGNORE_REJECTS, 1u);
        return;
    case BX_IGNORE_SOURCE_DOTIGNORE:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_DOTIGNORE_REJECTS, 1u);
        return;
    case BX_IGNORE_SOURCE_BUILTIN:
    default:
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_BUILTIN_REJECTS, 1u);
        return;
    }
}

static bool bx_ignore_rule_is_literal_basename(bool directory_only,
                                               const char *pattern) {
    return !directory_only &&
           pattern &&
           pattern[0] != '\0' &&
           strpbrk(pattern, "*?[\\") == NULL &&
           strchr(pattern, '/') == NULL;
}

static bool bx_ignore_rule_is_literal_extension(bool directory_only,
                                                const char *pattern) {
    return !directory_only &&
           pattern &&
           pattern[0] == '*' &&
           pattern[1] == '.' &&
           pattern[2] != '\0' &&
           strpbrk(pattern + 1, "*?[\\") == NULL &&
           strchr(pattern + 1, '/') == NULL;
}

static bool bx_ignore_rule_is_literal_directory(bool directory_only,
                                                const char *pattern) {
    return directory_only &&
           pattern &&
           pattern[0] != '\0' &&
           strpbrk(pattern, "*?[\\") == NULL &&
           strchr(pattern, '/') == NULL;
}

static bool bx_ignore_rule_is_anchored_prefix_candidate(bool uses_relative_path,
                                                        bool directory_only,
                                                        const char *pattern) {
    return (directory_only || uses_relative_path) &&
           pattern &&
           pattern[0] != '\0' &&
           strpbrk(pattern, "*?[\\") == NULL;
}

static bool bx_ignore_rule_uses_generic_glob_fallback(
    const struct bx_ignore_rule *rule) {
    return rule && rule->kind == BX_IGNORE_RULE_GENERIC_GLOB;
}

static enum bx_ignore_rule_kind
bx_ignore_rule_classify(bool uses_relative_path,
                        bool directory_only,
                        const char *pattern) {
    if (!pattern || pattern[0] == '\0')
        return BX_IGNORE_RULE_INVALID;
    if (bx_ignore_rule_is_literal_basename(directory_only, pattern)) {
        return BX_IGNORE_RULE_LITERAL_BASENAME;
    }
    if (bx_ignore_rule_is_literal_extension(directory_only, pattern)) {
        return BX_IGNORE_RULE_LITERAL_EXTENSION;
    }
    if (bx_ignore_rule_is_literal_directory(directory_only, pattern)) {
        return BX_IGNORE_RULE_LITERAL_DIRECTORY;
    }
    if (bx_ignore_rule_is_anchored_prefix_candidate(uses_relative_path,
                                                   directory_only,
                                                   pattern)) {
        return BX_IGNORE_RULE_ANCHORED_PREFIX;
    }
    return BX_IGNORE_RULE_GENERIC_GLOB;
}

static bool bx_ignore_rule_compile(struct bx_ignore_rule *rule,
                                   const char *line,
                                   enum bx_ignore_source_kind source) {
    const char *pattern = line;

    if (!rule || !line)
        return false;
    rule->source = bx_ignore_source_kind_is_valid(source)
        ? source
        : BX_IGNORE_SOURCE_BUILTIN;

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
    if (!rule->pattern)
        return false;
    rule->kind = bx_ignore_rule_classify(rule->uses_relative_path,
                                         rule->directory_only,
                                         rule->pattern);
    return rule->kind != BX_IGNORE_RULE_INVALID;
}

static void bx_ignore_program_build_source_masks(struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return;

    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        unsigned source_mask = bx_ignore_source_mask(rule->source);

        if (rule->kind == BX_IGNORE_RULE_LITERAL_BASENAME)
            program->literal_basename_source_mask |= source_mask;
        else if (rule->kind == BX_IGNORE_RULE_LITERAL_EXTENSION)
            program->literal_extension_source_mask |= source_mask;
        else if (rule->kind == BX_IGNORE_RULE_LITERAL_DIRECTORY) {
            program->literal_directory_source_mask |= source_mask;
            program->anchored_prefix_source_mask |= source_mask;
        } else if (rule->kind == BX_IGNORE_RULE_ANCHORED_PREFIX)
            program->anchored_prefix_source_mask |= source_mask;
        else if (rule->kind == BX_IGNORE_RULE_GENERIC_GLOB)
            program->generic_glob_source_mask |= source_mask;
    }
}

static bool bx_ignore_program_rules_are_basename_only(const struct bx_ignore_program *program) {
    if (!program || !program->rules || program->rule_count <= 0)
        return false;

    for (int i = 0; i < program->rule_count; ++i) {
        if (program->rules[i].kind != BX_IGNORE_RULE_LITERAL_BASENAME)
            return false;
    }
    return true;
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

    if (!program || !rule || rule->kind != BX_IGNORE_RULE_ANCHORED_PREFIX || !relative_path)
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

    if (rule->kind == BX_IGNORE_RULE_ANCHORED_PREFIX) {
        bx_ignore_note_source_checks(bx_ignore_source_mask(rule->source));
        if (bx_ignore_rule_matches_anchored_prefix(program, rule, relative_path, is_dir)) {
            if (!rule->negate)
                bx_ignore_note_source_reject(rule->source);
            return rule->negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE;
        }
        return BX_IGNORE_NO_MATCH;
    }

    if (program->casefold)
        flags |= FNM_CASEFOLD;
    if (!candidate || candidate[0] == '\0')
        return BX_IGNORE_NO_MATCH;
    if (rule->directory_only && !is_dir)
        return BX_IGNORE_NO_MATCH;

    bool generic_glob = bx_ignore_rule_uses_generic_glob_fallback(rule);
    bool count_generic_glob = false;
    /* Literal tables are accelerators only; generic glob rules still fall back to fnmatch(). */
    if (generic_glob) {
        count_generic_glob = bx_search_dev_counters_enabled();
        if (count_generic_glob) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS, 1u);
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_CHECKS, 1u);
        }
        bx_ignore_note_source_checks(bx_ignore_source_mask(rule->source));
    }
    if (fnmatch(rule->pattern, candidate, flags) != 0)
        return BX_IGNORE_NO_MATCH;
    if (generic_glob && !rule->negate) {
        if (count_generic_glob)
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GENERIC_GLOB_REJECTS, 1u);
        bx_ignore_note_source_reject(rule->source);
    }
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
bx_ignore_program_match_after_literal_basename(
    const struct bx_ignore_program *program,
    const char *name,
    const char *relative_path,
    bool is_dir,
    const struct bx_ignore_literal_basename *literal_basename) {
    if (!literal_basename)
        return BX_IGNORE_NO_MATCH;
    return bx_ignore_program_match_rules_from(program,
                                              name,
                                              relative_path,
                                              is_dir,
                                              program->rule_count - 1,
                                              literal_basename->rule_index + 1);
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_extension(
    const struct bx_ignore_program *program,
    const char *name,
    const char *relative_path,
    bool is_dir,
    const struct bx_ignore_literal_extension *literal_extension) {
    if (!literal_extension)
        return BX_IGNORE_NO_MATCH;
    return bx_ignore_program_match_rules_from(program,
                                              name,
                                              relative_path,
                                              is_dir,
                                              program->rule_count - 1,
                                              literal_extension->rule_index + 1);
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_literal_directory(
    const struct bx_ignore_program *program,
    const char *name,
    const char *relative_path,
    bool is_dir,
    const struct bx_ignore_literal_directory *literal_directory) {
    if (!literal_directory)
        return BX_IGNORE_NO_MATCH;
    return bx_ignore_program_match_rules_from(program,
                                              name,
                                              relative_path,
                                              is_dir,
                                              program->rule_count - 1,
                                              literal_directory->rule_index + 1);
}

static enum bx_ignore_match_result
bx_ignore_program_match_after_anchored_prefix(
    const struct bx_ignore_program *program,
    const char *name,
    const char *relative_path,
    bool is_dir,
    const struct bx_ignore_anchored_prefix *anchored_prefix) {
    if (!anchored_prefix)
        return BX_IGNORE_NO_MATCH;
    return bx_ignore_program_match_rules_from(program,
                                              name,
                                              relative_path,
                                              is_dir,
                                              program->rule_count - 1,
                                              anchored_prefix->rule_index + 1);
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
        if (rule->kind != BX_IGNORE_RULE_GENERIC_GLOB)
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
        if (program->rules[i].kind == BX_IGNORE_RULE_LITERAL_BASENAME)
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
        if (rule->kind != BX_IGNORE_RULE_LITERAL_BASENAME)
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
        if (program->rules[i].kind == BX_IGNORE_RULE_ANCHORED_PREFIX ||
            program->rules[i].kind == BX_IGNORE_RULE_LITERAL_DIRECTORY) {
            candidate_count++;
        }
    }
    if (candidate_count == 0)
        return true;

    program->anchored_prefixes =
        calloc((size_t)candidate_count, sizeof(*program->anchored_prefixes));
    if (!program->anchored_prefixes)
        return false;

    for (int i = 0; i < program->rule_count; ++i) {
        const struct bx_ignore_rule *rule = &program->rules[i];
        if (rule->kind != BX_IGNORE_RULE_ANCHORED_PREFIX &&
            rule->kind != BX_IGNORE_RULE_LITERAL_DIRECTORY) {
            continue;
        }
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
        if (program->rules[i].kind == BX_IGNORE_RULE_LITERAL_EXTENSION)
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
        if (rule->kind != BX_IGNORE_RULE_LITERAL_EXTENSION)
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
        if (program->rules[i].kind == BX_IGNORE_RULE_LITERAL_DIRECTORY)
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
        if (rule->kind != BX_IGNORE_RULE_LITERAL_DIRECTORY)
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

struct bx_ignore_program *
bx_ignore_program_compile_with_sources(char *const *patterns,
                                       const enum bx_ignore_source_kind *sources,
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
        enum bx_ignore_source_kind source = sources
            ? sources[i]
            : BX_IGNORE_SOURCE_BUILTIN;
        if (!bx_ignore_rule_compile(&program->rules[program->rule_count], patterns[i], source)) {
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

    program->casefold = casefold;
    program->basename_only = bx_ignore_program_rules_are_basename_only(program);
    bx_ignore_program_build_source_masks(program);
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
    bx_ignore_program_destroy(program);
    return NULL;
}

struct bx_ignore_program *bx_ignore_program_compile(char *const *patterns,
                                                    int pattern_count,
                                                    bool casefold) {
    return bx_ignore_program_compile_with_sources(patterns, NULL, pattern_count, casefold);
}

void bx_ignore_program_release(struct bx_ignore_program *program) {
    if (!program)
        return;
    if (program->process_lifetime)
        return;
    bx_ignore_program_destroy(program);
}

void bx_ignore_program_make_process_lifetime(struct bx_ignore_program *program) {
    if (program)
        program->process_lifetime = true;
}

bool bx_ignore_program_is_process_lifetime(const struct bx_ignore_program *program) {
    return program && program->process_lifetime;
}

void bx_ignore_program_destroy_process_lifetime(struct bx_ignore_program *program) {
    if (!program)
        return;
    program->process_lifetime = false;
    bx_ignore_program_destroy(program);
}

bool bx_ignore_program_is_basename_only(const struct bx_ignore_program *program) {
    return program && program->basename_only;
}

bool bx_ignore_program_has_generic_glob_fallback(const struct bx_ignore_program *program) {
    return program && program->generic_glob_source_mask != 0u;
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_basename(const struct bx_ignore_program *program,
                                         const char *name,
                                         const char *relative_path,
                                         bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;
    bool count_literal_basename = program->literal_basenames
                                  && program->literal_basename_count > 0
                                  && bx_search_dev_counters_enabled();
    if (count_literal_basename) {
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_CHECKS, 1u);
        bx_ignore_note_source_checks(program->literal_basename_source_mask);
    }

    const struct bx_ignore_literal_basename *literal_basename =
        bx_ignore_program_find_literal_basename(program, name);
    if (literal_basename) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_after_literal_basename(program,
                                                           name,
                                                           relative_path,
                                                           is_dir,
                                                           literal_basename);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        if (literal_basename->result == BX_IGNORE_EXCLUDE && count_literal_basename) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_LITERAL_BASENAME_REJECTS, 1u);
            bx_ignore_note_source_reject(program->rules[literal_basename->rule_index].source);
        }
        return literal_basename->result;
    }
    return BX_IGNORE_NO_MATCH;
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_extension(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;
    bool count_literal_extension = program->literal_extensions
                                   && program->literal_extension_count > 0
                                   && bx_search_dev_counters_enabled();
    if (count_literal_extension) {
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_CHECKS, 1u);
        bx_ignore_note_source_checks(program->literal_extension_source_mask);
    }

    const struct bx_ignore_literal_extension *literal_extension =
        bx_ignore_program_find_literal_extension(program, name);
    if (literal_extension) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_after_literal_extension(program,
                                                            name,
                                                            relative_path,
                                                            is_dir,
                                                            literal_extension);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        if (literal_extension->result == BX_IGNORE_EXCLUDE && count_literal_extension) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_LITERAL_EXTENSION_REJECTS, 1u);
            bx_ignore_note_source_reject(program->rules[literal_extension->rule_index].source);
        }
        return literal_extension->result;
    }
    return BX_IGNORE_NO_MATCH;
}

enum bx_ignore_match_result
bx_ignore_program_match_literal_directory(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;
    bool count_literal_directory = program->literal_directories
                                   && program->literal_directory_count > 0
                                   && bx_search_dev_counters_enabled();
    if (count_literal_directory)
        bx_ignore_note_source_checks(program->literal_directory_source_mask);

    const struct bx_ignore_literal_directory *literal_directory =
        is_dir ? bx_ignore_program_find_literal_directory(program, name) : NULL;
    if (literal_directory) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_after_literal_directory(program,
                                                            name,
                                                            relative_path,
                                                            is_dir,
                                                            literal_directory);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        if (literal_directory->result == BX_IGNORE_EXCLUDE && count_literal_directory)
            bx_ignore_note_source_reject(program->rules[literal_directory->rule_index].source);
        return literal_directory->result;
    }
    return BX_IGNORE_NO_MATCH;
}

enum bx_ignore_match_result
bx_ignore_program_match_anchored_prefix(const struct bx_ignore_program *program,
                                        const char *name,
                                        const char *relative_path,
                                        bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;
    bool count_anchored_prefix = program->anchored_prefixes
                                 && program->anchored_prefix_count > 0
                                 && bx_search_dev_counters_enabled();
    if (count_anchored_prefix) {
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_CHECKS, 1u);
        bx_ignore_note_source_checks(program->anchored_prefix_source_mask);
    }

    const struct bx_ignore_anchored_prefix *anchored_prefix =
        bx_ignore_program_find_anchored_prefix(program, relative_path, is_dir);
    if (anchored_prefix) {
        enum bx_ignore_match_result later =
            bx_ignore_program_match_after_anchored_prefix(program,
                                                          name,
                                                          relative_path,
                                                          is_dir,
                                                          anchored_prefix);
        if (later != BX_IGNORE_NO_MATCH)
            return later;
        if (anchored_prefix->result == BX_IGNORE_EXCLUDE && count_anchored_prefix) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_ANCHORED_PREFIX_REJECTS, 1u);
            bx_ignore_note_source_reject(program->rules[anchored_prefix->rule_index].source);
        }
        return anchored_prefix->result;
    }
    return BX_IGNORE_NO_MATCH;
}

enum bx_ignore_match_result
bx_ignore_program_match_generic_glob_fallback(const struct bx_ignore_program *program,
                                              const char *name,
                                              const char *relative_path,
                                              bool is_dir) {
    if (!program || !program->rules || program->rule_count <= 0)
        return BX_IGNORE_NO_MATCH;
    if (!bx_ignore_program_has_generic_glob_fallback(program))
        return BX_IGNORE_NO_MATCH;

    /* All literal and anchored-prefix rules were already staged earlier; only generic glob rules remain here. */
    return bx_ignore_program_match_generic_rules_from(program,
                                                      name,
                                                      relative_path,
                                                      is_dir,
                                                      program->rule_count - 1,
                                                      0);
}

enum bx_ignore_match_result
bx_ignore_program_match_without_generic_glob_fallback(const struct bx_ignore_program *program,
                                                      const char *name,
                                                      const char *relative_path,
                                                      bool is_dir) {
    enum bx_ignore_match_result literal_basename =
        bx_ignore_program_match_literal_basename(program, name, relative_path, is_dir);
    if (literal_basename != BX_IGNORE_NO_MATCH)
        return literal_basename;

    enum bx_ignore_match_result literal_extension =
        bx_ignore_program_match_literal_extension(program, name, relative_path, is_dir);
    if (literal_extension != BX_IGNORE_NO_MATCH)
        return literal_extension;

    enum bx_ignore_match_result literal_directory =
        bx_ignore_program_match_literal_directory(program, name, relative_path, is_dir);
    if (literal_directory != BX_IGNORE_NO_MATCH)
        return literal_directory;

    return bx_ignore_program_match_anchored_prefix(program, name, relative_path, is_dir);
}

enum bx_ignore_match_result bx_ignore_program_match(const struct bx_ignore_program *program,
                                                    const char *name,
                                                    const char *relative_path,
                                                    bool is_dir) {
    enum bx_ignore_match_result result =
        bx_ignore_program_match_without_generic_glob_fallback(program,
                                                              name,
                                                              relative_path,
                                                              is_dir);
    if (result != BX_IGNORE_NO_MATCH)
        return result;
    return bx_ignore_program_match_generic_glob_fallback(program, name, relative_path, is_dir);
}
