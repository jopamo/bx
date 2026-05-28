#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"
#include "traverse.h"

struct bx_search_walk_dir_frame {
    struct bx_ignore_state *owned_ignore_state;
    struct bx_ignore_state *ignore_state;
};

struct bx_search_walk_state {
    const struct bx_search_walk_config *config;
    void *user;
    struct bx_walk_filter_state filter_state;
    struct bx_walk_ignore_opts ignore_opts;
    bool have_ignore_opts;
    char *git_root_owned;
    struct bx_search_walk_dir_frame *dir_frames;
    size_t dir_frame_len;
    size_t dir_frame_cap;
};

static bool bx_search_walk_dir_frames_reserve(struct bx_search_walk_state *state, size_t needed) {
    if (state->dir_frame_cap >= needed)
        return true;

    size_t new_cap = state->dir_frame_cap == 0 ? 8u : state->dir_frame_cap * 2u;
    while (new_cap < needed)
        new_cap *= 2u;

    struct bx_search_walk_dir_frame *tmp =
        realloc(state->dir_frames, new_cap * sizeof(*state->dir_frames));
    if (!tmp)
        return false;

    state->dir_frames = tmp;
    state->dir_frame_cap = new_cap;
    return true;
}

static void bx_search_walk_dir_frames_pop_to_depth(struct bx_search_walk_state *state, int depth) {
    size_t keep = depth < 0 ? 0u : (size_t)depth + 1u;

    while (state->dir_frame_len > keep) {
        struct bx_search_walk_dir_frame *top =
            &state->dir_frames[state->dir_frame_len - 1u];
        if (top->owned_ignore_state) {
            bx_ignore_state_dispose(top->owned_ignore_state);
            free(top->owned_ignore_state);
        }
        state->dir_frame_len--;
    }
}

static struct bx_ignore_state *
bx_search_walk_active_ignore_state(const struct bx_search_walk_state *state, int depth) {
    if (!state || depth < 0 || state->dir_frame_len == 0u)
        return NULL;
    if ((size_t)depth >= state->dir_frame_len)
        return NULL;
    return state->dir_frames[depth].ignore_state;
}

static bool bx_search_walk_init_root_dir_frame(struct bx_search_walk_state *state,
                                               struct bx_ignore_state *parent_ignore_state) {
    if (!state || !state->have_ignore_opts)
        return true;
    if (!bx_search_walk_dir_frames_reserve(state, 1u))
        return false;

    state->dir_frames[0] = (struct bx_search_walk_dir_frame){
        .owned_ignore_state = parent_ignore_state,
        .ignore_state = parent_ignore_state,
    };
    state->dir_frame_len = 1u;
    return true;
}

static enum bx_walk_action bx_search_walk_push_dir_frame(struct bx_search_walk_state *state,
                                                         struct bx_walk_entry *entry,
                                                         int depth) {
    struct bx_ignore_state *parent = bx_search_walk_active_ignore_state(state, depth);
    struct bx_ignore_state *head = parent;
    struct bx_ignore_state *owned = NULL;

    if (state->have_ignore_opts && !state->ignore_opts.no_ignore) {
        struct bx_ignore_program *program =
            bx_ignore_load_program(entry->path, &state->ignore_opts);
        if (program) {
            owned = calloc(1u, sizeof(*owned));
            if (!owned) {
                bx_ignore_program_release(program);
                return BX_WALK_ERROR;
            }
            bx_ignore_state_init(owned, parent, entry->path, program);
            owned->owned_dirpath = strdup(entry->path);
            if (!owned->owned_dirpath) {
                bx_ignore_state_dispose(owned);
                free(owned);
                return BX_WALK_ERROR;
            }
            owned->dirpath = owned->owned_dirpath;
            head = owned;
        }
    }

    if (!bx_search_walk_dir_frames_reserve(state, (size_t)depth + 2u)) {
        if (owned) {
            bx_ignore_state_dispose(owned);
            free(owned);
        }
        return BX_WALK_ERROR;
    }

    state->dir_frames[state->dir_frame_len++] = (struct bx_search_walk_dir_frame){
        .owned_ignore_state = owned,
        .ignore_state = head,
    };
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_walk_visit(struct bx_walk_entry *entry, void *user) {
    struct bx_search_walk_state *state = user;
    bx_search_walk_dir_frames_pop_to_depth(state, entry->depth);

    const char *name = bx_path_basename_ptr(entry->path);
    struct bx_ignore_state *ignore_state =
        bx_search_walk_active_ignore_state(state, entry->depth);
    bool entry_selected = true;

    if (entry->depth > 0 &&
        bx_walk_filter_should_skip(&state->filter_state, entry, ignore_state, &entry_selected)) {
        return entry->is_dir ? BX_WALK_PRUNE : BX_WALK_CONTINUE;
    }

    if (entry_selected && entry->depth > 0 &&
        state->config->filter_opts &&
        state->config->filter_opts->num_include_patterns > 0 &&
        !entry->is_dir &&
        !bx_walk_filter_matches_include(&state->filter_state, name, entry->path)) {
        return BX_WALK_CONTINUE;
    }

    enum bx_walk_action action = BX_WALK_CONTINUE;
    if (!entry_selected) {
        action = BX_WALK_CONTINUE;
    } else if (state->config->visit_with_ignore) {
        action = state->config->visit_with_ignore(entry,
                                                  ignore_state,
                                                  state->have_ignore_opts ? &state->ignore_opts : NULL,
                                                  state->user);
    } else {
        action = state->config->visit(entry, state->user);
    }
    if (action == BX_WALK_CONTINUE && entry->prune)
        action = BX_WALK_PRUNE;

    if (state->have_ignore_opts && entry->is_dir && action == BX_WALK_CONTINUE) {
        enum bx_walk_action push_action =
            bx_search_walk_push_dir_frame(state, entry, entry->depth);
        if (push_action != BX_WALK_CONTINUE)
            return push_action;
    }

    return action;
}

static enum bx_walk_action bx_search_walk_error(const char *path, int errnum, void *user) {
    struct bx_search_walk_state *state = user;
    return state->config->error(path, errnum, state->user);
}

int bx_search_walk(const char *root,
                   const struct bx_search_walk_config *config,
                   void *user) {
    if (!root || !config || !config->walk_opts ||
        (!config->visit && !config->visit_with_ignore)) {
        errno = EINVAL;
        return -1;
    }

    struct bx_search_walk_state state = {
        .config = config,
        .user = user,
    };
    bx_walk_filter_init(&state.filter_state, config->filter_opts, root);

    if (config->ignore_opts) {
        state.ignore_opts = *config->ignore_opts;
        state.have_ignore_opts = true;
        if (state.ignore_opts.git_root) {
            state.git_root_owned = strdup(state.ignore_opts.git_root);
            if (!state.git_root_owned) {
                free(state.dir_frames);
                return -1;
            }
            state.ignore_opts.git_root = state.git_root_owned;
        } else {
            state.ignore_opts.gitignore_enabled = bx_ignore_enable_gitignore_for_root(root,
                                                                                      &state.ignore_opts);
            state.git_root_owned = bx_ignore_find_git_root(root, &state.ignore_opts);
            state.ignore_opts.git_root = state.git_root_owned;
        }

        if (config->inherited_parent_ignore_state) {
            if (!bx_search_walk_init_root_dir_frame(&state,
                                                    config->inherited_parent_ignore_state)) {
                bx_ignore_state_dispose_chain(config->inherited_parent_ignore_state);
                free(state.git_root_owned);
                free(state.dir_frames);
                return -1;
            }
        } else {
            bool ok = false;
            struct bx_ignore_state *parent_ignore_state =
                bx_ignore_load_parent_state(root, &state.ignore_opts, &ok);
            if (!ok) {
                bx_ignore_state_dispose_chain(parent_ignore_state);
                free(state.git_root_owned);
                free(state.dir_frames);
                return -1;
            }
            if (!bx_search_walk_init_root_dir_frame(&state, parent_ignore_state)) {
                bx_ignore_state_dispose_chain(parent_ignore_state);
                free(state.git_root_owned);
                free(state.dir_frames);
                return -1;
            }
        }
    }

    struct bx_walk_ops ops = {
        .visit = bx_search_walk_visit,
        .error = config->error ? bx_search_walk_error : NULL,
    };
    int rc = bx_walk(root, config->walk_opts, &ops, &state);

    bx_search_walk_dir_frames_pop_to_depth(&state, -1);
    free(state.git_root_owned);
    free(state.dir_frames);
    return rc;
}
