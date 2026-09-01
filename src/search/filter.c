#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "dev_counters.h"
#include "fswalk/walk.h"
#include "filter.h"
#include "ignore.h"
#include "lib/path_ops.h"
#include "metadata.h"

static const char *bx_walk_filter_relative_path(const struct bx_walk_filter_state *state,
                                                const char *path);
static bool bx_walk_filter_matches_include_relative(const struct bx_walk_filter_state *state,
                                                    const char *name,
                                                    const char *relative_path);

static bool bx_walk_filter_is_hidden(const char *name) {
    return name[0] == '.';
}

static bool bx_walk_filter_hidden_policy_should_skip(const struct bx_walk_filter_state *state,
                                                     const char *name,
                                                     const char *path,
                                                     const char **relative_path_out) {
    const char *relative_path;

    if (relative_path_out)
        *relative_path_out = NULL;
    if (!state || !state->opts || state->opts->hidden)
        return false;
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_CHECKS, 1u);
    if (!bx_walk_filter_is_hidden(name))
        return false;

    relative_path = bx_walk_filter_relative_path(state, path);
    if (relative_path_out)
        *relative_path_out = relative_path;
    if (bx_walk_filter_matches_include_relative(state, name, relative_path))
        return false;
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_HIDDEN_POLICY_REJECTS, 1u);
    return true;
}

static const char *bx_walk_filter_relative_path(const struct bx_walk_filter_state *state,
                                                const char *path) {
    if (!state || !state->root_path || !path)
        return path;

    size_t root_len = state->root_path_len;
    if (strncmp(path, state->root_path, root_len) != 0)
        return path;
    if (path[root_len] == '/')
        return path + root_len + 1;
    if (path[root_len] == '\0')
        return path + root_len;
    return path;
}

static bool bx_walk_filter_matches_include_relative(const struct bx_walk_filter_state *state,
                                                    const char *name,
                                                    const char *relative_path) {
    if (!state || !state->opts || !state->opts->include_patterns ||
        state->opts->num_include_patterns == 0u) {
        return false;
    }

    for (size_t i = 0u; i < state->opts->num_include_patterns; i++) {
        const char *pattern = state->opts->include_patterns[i];
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        bool type_pattern = state->opts->include_pattern_is_type &&
                            state->opts->include_pattern_is_type[i];
        bx_search_dev_counters_note_walk(type_pattern
                                             ? BX_SEARCH_WALK_FILTER_TYPE_POLICY_CHECKS
                                             : BX_SEARCH_WALK_FILTER_CLI_GLOB_CHECKS,
                                         1u);
        if (!pattern || pattern[0] == '\0')
            continue;
        if (state->opts->include_pattern_casefold &&
            state->opts->include_pattern_casefold[i]) {
            flags |= FNM_CASEFOLD;
        }
        if (fnmatch(pattern, name, flags) == 0)
            return true;
        if (relative_path && relative_path[0] != '\0') {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS, 1u);
            if (fnmatch(pattern, relative_path, FNM_PATHNAME | flags) == 0)
                return true;
        }
    }

    return false;
}

static void bx_walk_filter_note_include_reject(const struct bx_walk_filter_state *state) {
    bool noted_type = false;
    bool noted_cli = false;

    if (!state || !state->opts || !state->opts->include_patterns ||
        state->opts->num_include_patterns == 0u) {
        return;
    }

    for (size_t i = 0u; i < state->opts->num_include_patterns; i++) {
        bool type_pattern = state->opts->include_pattern_is_type &&
                            state->opts->include_pattern_is_type[i];
        if (type_pattern) {
            if (!noted_type) {
                bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS, 1u);
                noted_type = true;
            }
        } else if (!noted_cli) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS, 1u);
            noted_cli = true;
        }
    }
}

static bool bx_walk_filter_matches_exclude(const struct bx_walk_filter_state *state,
                                           const char *name,
                                           const char *relative_path) {
    if (!state || !state->opts || !state->opts->exclude_patterns ||
        state->opts->num_exclude_patterns == 0u) {
        return false;
    }

    for (size_t i = 0u; i < state->opts->num_exclude_patterns; i++) {
        const char *pattern = state->opts->exclude_patterns[i];
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        bool type_pattern = state->opts->exclude_pattern_is_type &&
                            state->opts->exclude_pattern_is_type[i];
        bx_search_dev_counters_note_walk(type_pattern
                                             ? BX_SEARCH_WALK_FILTER_TYPE_POLICY_CHECKS
                                             : BX_SEARCH_WALK_FILTER_CLI_GLOB_CHECKS,
                                         1u);
        if (!pattern || pattern[0] == '\0')
            continue;
        if (fnmatch(pattern, name, flags) == 0) {
            bx_search_dev_counters_note_walk(type_pattern
                                                 ? BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS
                                                 : BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS,
                                             1u);
            return true;
        }
        if (relative_path && relative_path[0] != '\0') {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GLOB_FALLBACKS, 1u);
            if (fnmatch(pattern, relative_path, FNM_PATHNAME | flags) == 0) {
                bx_search_dev_counters_note_walk(type_pattern
                                                     ? BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS
                                                     : BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS,
                                                 1u);
                return true;
            }
        }
    }

    return false;
}

static bool bx_walk_filter_matches_exclude_dir(const struct bx_walk_filter_state *state,
                                               const char *name) {
    if (!state || !state->opts || !state->opts->exclude_dirs)
        return false;
    if (state->opts->num_exclude_dirs == 0u)
        return false;
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_CLI_GLOB_CHECKS, 1u);

    for (size_t i = 0u; i < state->opts->num_exclude_dirs; i++) {
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (fnmatch(state->opts->exclude_dirs[i], name, flags) == 0) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_CLI_GLOB_REJECTS, 1u);
            return true;
        }
    }

    return false;
}

static bool bx_walk_filter_entry_matches_type(const struct bx_walk_filter_state *state,
                                              struct bx_walk_entry *entry) {
    if (!state || !state->opts || state->opts->type_filter == '\0')
        return true;
    return bx_walk_entry_matches_type(entry, state->opts->type_filter);
}

static enum bx_walk_type_match_state
bx_walk_filter_entry_matches_type_without_metadata(const struct bx_walk_filter_state *state,
                                                   struct bx_walk_entry *entry) {
    if (!state || !state->opts || state->opts->type_filter == '\0')
        return BX_WALK_TYPE_MATCH_YES;
    return bx_walk_entry_matches_type_without_metadata(entry, state->opts->type_filter);
}

void bx_walk_filter_init(struct bx_walk_filter_state *state,
                         const struct bx_walk_filter_opts *opts,
                         const char *root_path) {
    if (!state)
        return;
    state->opts = opts;
    state->root_path = root_path;
    state->root_path_len = root_path ? strlen(root_path) : 0u;
}

bool bx_walk_filter_should_skip(const struct bx_walk_filter_state *state,
                                struct bx_walk_entry *entry,
                                const struct bx_ignore_state *ignore_state,
                                bool *entry_selected_out) {
    const char *relative_path = NULL;
    const char *name;
    const char *path;
    bool entry_selected = true;
    bool metadata_type_pending = false;
    bool basename_only_fast_path = false;

    if (entry_selected_out)
        *entry_selected_out = true;
    if (!entry || !entry->path)
        return false;

    name = bx_path_basename_ptr(entry->path);
    path = entry->path;

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_CHECKS, 1u);

    if (bx_walk_filter_hidden_policy_should_skip(state, name, path, &relative_path))
        return true;

    enum bx_walk_type_match_state type_match =
        bx_walk_filter_entry_matches_type_without_metadata(state, entry);
    if (state && state->opts && state->opts->type_filter != '\0')
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_TYPE_POLICY_CHECKS, 1u);

    if (type_match == BX_WALK_TYPE_MATCH_NO) {
        entry_selected = false;
        if (!entry->is_dir) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS, 1u);
            if (entry_selected_out)
                *entry_selected_out = false;
            return false;
        }
    } else if (type_match == BX_WALK_TYPE_MATCH_DEFER_METADATA) {
        metadata_type_pending = true;
    }

    if (!relative_path)
        relative_path = bx_walk_filter_relative_path(state, path);

    enum bx_ignore_match_result basename_ignore =
        bx_ignore_state_match_literal_basename(ignore_state,
                                               name,
                                               path,
                                               relative_path,
                                               entry->is_dir);
    if (basename_ignore == BX_IGNORE_EXCLUDE)
        return true;

    enum bx_ignore_match_result extension_ignore = BX_IGNORE_NO_MATCH;
    enum bx_ignore_match_result directory_ignore = BX_IGNORE_NO_MATCH;
    enum bx_ignore_match_result anchored_prefix_ignore = BX_IGNORE_NO_MATCH;

    if (basename_ignore == BX_IGNORE_NO_MATCH) {
        if (bx_ignore_state_is_basename_only_chain(ignore_state)) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_BASENAME_ONLY_FAST_PATHS, 1u);
            basename_only_fast_path = true;
            goto ignore_complete;
        }

        extension_ignore = bx_ignore_state_match_literal_extension(ignore_state,
                                                                   name,
                                                                   path,
                                                                   relative_path,
                                                                   entry->is_dir);
        if (extension_ignore == BX_IGNORE_EXCLUDE)
            return true;
    }

    if (basename_ignore == BX_IGNORE_NO_MATCH &&
        extension_ignore == BX_IGNORE_NO_MATCH) {
        directory_ignore = bx_ignore_state_match_literal_directory(ignore_state,
                                                                  name,
                                                                  path,
                                                                  relative_path,
                                                                  entry->is_dir);
        if (directory_ignore == BX_IGNORE_EXCLUDE)
            return true;
    }

    if (basename_ignore == BX_IGNORE_NO_MATCH &&
        extension_ignore == BX_IGNORE_NO_MATCH &&
        directory_ignore == BX_IGNORE_NO_MATCH) {
        anchored_prefix_ignore = bx_ignore_state_match_anchored_prefix(ignore_state,
                                                                      name,
                                                                      path,
                                                                      relative_path,
                                                                      entry->is_dir);
        if (anchored_prefix_ignore == BX_IGNORE_EXCLUDE)
            return true;
    }

ignore_complete:
    if (metadata_type_pending) {
        entry_selected = bx_walk_filter_entry_matches_type(state, entry);
        if (!entry_selected && !entry->is_dir) {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_TYPE_POLICY_REJECTS, 1u);
            if (entry_selected_out)
                *entry_selected_out = false;
            return false;
        }
    }

    if (!basename_only_fast_path &&
        basename_ignore == BX_IGNORE_NO_MATCH &&
        extension_ignore == BX_IGNORE_NO_MATCH &&
        directory_ignore == BX_IGNORE_NO_MATCH &&
        anchored_prefix_ignore == BX_IGNORE_NO_MATCH) {
        if (bx_ignore_state_has_generic_glob_fallback_chain(ignore_state)) {
            if (bx_ignore_state_match_generic_glob_fallback(ignore_state,
                                                           name,
                                                           path,
                                                           relative_path,
                                                           entry->is_dir) == BX_IGNORE_EXCLUDE) {
                return true;
            }
        } else {
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_NO_GENERIC_GLOB_FAST_PATHS,
                                             1u);
        }
    }

    if (bx_walk_filter_matches_exclude_dir(state, name))
        return true;

    if (bx_walk_filter_matches_exclude(state, name, relative_path))
        return true;

    if (entry_selected_out)
        *entry_selected_out = entry_selected;
    return false;
}

bool bx_walk_filter_matches_include(const struct bx_walk_filter_state *state,
                                    const char *name,
                                    const char *path) {
    const char *relative_path = bx_walk_filter_relative_path(state, path);
    bool matched = bx_walk_filter_matches_include_relative(state, name, relative_path);
    if (!matched)
        bx_walk_filter_note_include_reject(state);
    return matched;
}
