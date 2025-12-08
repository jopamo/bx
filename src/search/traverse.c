#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "filter.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"
#include "traverse.h"

struct bx_search_walk_stack_entry {
    struct bx_ignore_state *owned;
    struct bx_ignore_state *head;
};

struct bx_search_walk_state {
    const struct bx_search_walk_config *config;
    void *user;
    struct bx_walk_filter_state filter_state;
    struct bx_walk_ignore_opts ignore_opts;
    bool have_ignore_opts;
    struct bx_ignore_state *parent_ignore_state;
    struct bx_search_walk_stack_entry *stack;
    size_t stack_len;
    size_t stack_cap;
};

static bool bx_search_walk_stack_reserve(struct bx_search_walk_state *state, size_t needed) {
    if (state->stack_cap >= needed)
        return true;

    size_t new_cap = state->stack_cap == 0 ? 8u : state->stack_cap * 2u;
    while (new_cap < needed)
        new_cap *= 2u;

    struct bx_search_walk_stack_entry *tmp = realloc(state->stack, new_cap * sizeof(*state->stack));
    if (!tmp)
        return false;

    state->stack = tmp;
    state->stack_cap = new_cap;
    return true;
}

static void bx_search_walk_stack_pop_to_depth(struct bx_search_walk_state *state, int depth) {
    while (state->stack_len > (size_t)depth) {
        struct bx_search_walk_stack_entry *top = &state->stack[state->stack_len - 1u];
        if (top->owned) {
            bx_ignore_state_dispose(top->owned);
            free(top->owned);
        }
        state->stack_len--;
    }
}

static struct bx_ignore_state *bx_search_walk_parent_ignore(const struct bx_search_walk_state *state,
                                                            int depth) {
    if (depth <= 0 || state->stack_len == 0u)
        return state->parent_ignore_state;
    return state->stack[depth - 1].head;
}

static enum bx_walk_action bx_search_walk_push_dir_state(struct bx_search_walk_state *state,
                                                         struct bx_walk_entry *entry,
                                                         int depth) {
    struct bx_ignore_state *parent = bx_search_walk_parent_ignore(state, depth);
    struct bx_ignore_state *head = parent;
    struct bx_ignore_state *owned = NULL;

    if (state->have_ignore_opts && !state->ignore_opts.no_ignore) {
        char **patterns = NULL;
        int pattern_count = 0;
        bx_ignore_load_patterns(entry->path, &state->ignore_opts, &patterns, &pattern_count);
        if (pattern_count > 0) {
            owned = calloc(1u, sizeof(*owned));
            if (!owned) {
                bx_ignore_free_patterns(patterns, pattern_count);
                return BX_WALK_ERROR;
            }
            bx_ignore_state_init(owned, parent, entry->path, patterns, pattern_count);
            head = owned;
        }
    }

    if (!bx_search_walk_stack_reserve(state, (size_t)depth + 1u)) {
        if (owned) {
            bx_ignore_state_dispose(owned);
            free(owned);
        }
        return BX_WALK_ERROR;
    }

    state->stack[state->stack_len++] = (struct bx_search_walk_stack_entry){
        .owned = owned,
        .head = head,
    };
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_walk_visit(struct bx_walk_entry *entry, void *user) {
    struct bx_search_walk_state *state = user;
    bx_search_walk_stack_pop_to_depth(state, entry->depth);

    const char *name = bx_path_basename_ptr(entry->path);
    struct bx_ignore_state *ignore_state = bx_search_walk_parent_ignore(state, entry->depth);

    if (entry->depth > 0 &&
        bx_walk_filter_should_skip(&state->filter_state, name, entry->path, ignore_state)) {
        return entry->is_dir ? BX_WALK_PRUNE : BX_WALK_CONTINUE;
    }

    if (entry->depth > 0 &&
        state->config->filter_opts &&
        state->config->filter_opts->num_include_patterns > 0 &&
        !entry->is_dir &&
        !bx_walk_filter_matches_include(&state->filter_state, name, entry->path)) {
        return BX_WALK_CONTINUE;
    }

    enum bx_walk_action action = state->config->visit(entry, state->user);
    if (action == BX_WALK_CONTINUE && entry->prune)
        action = BX_WALK_PRUNE;

    if (entry->is_dir && action == BX_WALK_CONTINUE) {
        enum bx_walk_action push_action = bx_search_walk_push_dir_state(state, entry, entry->depth);
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
    if (!root || !config || !config->walk_opts || !config->visit) {
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
        state.ignore_opts.gitignore_enabled = bx_ignore_enable_gitignore_for_root(root, &state.ignore_opts);
        state.have_ignore_opts = true;
        bool ok = false;
        state.parent_ignore_state = bx_ignore_load_parent_state(root, &state.ignore_opts, &ok);
        if (!ok) {
            bx_ignore_state_dispose_chain(state.parent_ignore_state);
            free(state.stack);
            return -1;
        }
    }

    struct bx_walk_ops ops = {
        .visit = bx_search_walk_visit,
        .error = config->error ? bx_search_walk_error : NULL,
    };
    int rc = bx_walk(root, config->walk_opts, &ops, &state);

    bx_search_walk_stack_pop_to_depth(&state, 0);
    bx_ignore_state_dispose_chain(state.parent_ignore_state);
    free(state.stack);
    return rc;
}
