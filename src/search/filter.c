#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "dev_counters.h"
#include "fswalk/walk.h"
#include "filter.h"
#include "ignore.h"

static bool bx_walk_filter_is_hidden(const char *name) {
    return name[0] == '.';
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
        state->opts->num_include_patterns <= 0) {
        return false;
    }

    for (int i = 0; i < state->opts->num_include_patterns; i++) {
        const char *pattern = state->opts->include_patterns[i];
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
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

static bool bx_walk_filter_matches_exclude(const struct bx_walk_filter_state *state,
                                           const char *name,
                                           const char *relative_path) {
    if (!state || !state->opts || !state->opts->exclude_patterns ||
        state->opts->num_exclude_patterns <= 0) {
        return false;
    }

    for (int i = 0; i < state->opts->num_exclude_patterns; i++) {
        const char *pattern = state->opts->exclude_patterns[i];
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (!pattern || pattern[0] == '\0')
            continue;
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

static bool bx_walk_filter_matches_exclude_dir(const struct bx_walk_filter_state *state,
                                               const char *name) {
    if (!state || !state->opts || !state->opts->exclude_dirs)
        return false;

    for (int i = 0; i < state->opts->num_exclude_dirs; i++) {
        int flags = state->opts->glob_case_insensitive ? FNM_CASEFOLD : 0;
        if (fnmatch(state->opts->exclude_dirs[i], name, flags) == 0)
            return true;
    }

    return false;
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
                                const char *name,
                                const char *path,
                                const struct bx_ignore_state *ignore_state) {
    const char *relative_path = bx_walk_filter_relative_path(state, path);

    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_CHECKS, 1u);

    if (state && state->opts && !state->opts->hidden && bx_walk_filter_is_hidden(name) &&
        !bx_walk_filter_matches_include_relative(state, name, relative_path)) {
        return true;
    }

    if (bx_ignore_state_matches_path(ignore_state, name, path, relative_path))
        return true;

    if (bx_walk_filter_matches_exclude_dir(state, name))
        return true;

    if (bx_walk_filter_matches_exclude(state, name, relative_path))
        return true;

    return false;
}

bool bx_walk_filter_matches_include(const struct bx_walk_filter_state *state,
                                    const char *name,
                                    const char *path) {
    const char *relative_path = bx_walk_filter_relative_path(state, path);
    return bx_walk_filter_matches_include_relative(state, name, relative_path);
}
