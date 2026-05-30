#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dev_counters.h"
#include "filter.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"
#include "lib/time_parse.h"
#include "traverse.h"

struct bx_search_walk_dir_frame {
    int applies_depth;
    struct bx_ignore_state *owned_ignore_chain;
    const struct bx_ignore_state *ignore_state;
    struct bx_ignore_state inline_ignore_state;
    bool have_inline_ignore_state;
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

    struct bx_search_walk_dir_frame *old_frames = state->dir_frames;
    size_t old_len = state->dir_frame_len;
    uintptr_t old_base = (uintptr_t)old_frames;
    size_t new_cap = state->dir_frame_cap == 0 ? 8u : state->dir_frame_cap * 2u;
    while (new_cap < needed)
        new_cap *= 2u;

    struct bx_search_walk_dir_frame *tmp =
        realloc(state->dir_frames, new_cap * sizeof(*state->dir_frames));
    if (!tmp)
        return false;

    if (old_frames && tmp != old_frames) {
        uintptr_t new_base = (uintptr_t)tmp;
        for (size_t i = 0u; i < old_len; i++) {
            if (tmp[i].have_inline_ignore_state) {
                tmp[i].ignore_state = &tmp[i].inline_ignore_state;
                const struct bx_ignore_state *parent = tmp[i].inline_ignore_state.parent;
                if (parent) {
                    uintptr_t parent_addr = (uintptr_t)parent;
                    for (size_t j = 0u; j < old_len; j++) {
                        uintptr_t old_inline_addr = old_base +
                            j * sizeof(*tmp) +
                            offsetof(struct bx_search_walk_dir_frame, inline_ignore_state);
                        if (parent_addr == old_inline_addr) {
                            tmp[i].inline_ignore_state.parent =
                                (const struct bx_ignore_state *)(void *)(new_base +
                                    j * sizeof(*tmp) +
                                    offsetof(struct bx_search_walk_dir_frame,
                                             inline_ignore_state));
                            break;
                        }
                    }
                }
            }
        }
    }

    state->dir_frames = tmp;
    state->dir_frame_cap = new_cap;
    return true;
}

static void bx_search_walk_dir_frames_pop_to_depth(struct bx_search_walk_state *state, int depth) {
    while (state->dir_frame_len > 0u) {
        struct bx_search_walk_dir_frame *top =
            &state->dir_frames[state->dir_frame_len - 1u];
        if (top->applies_depth <= depth)
            break;
        if (top->have_inline_ignore_state)
            bx_ignore_state_dispose(&top->inline_ignore_state);
        if (top->owned_ignore_chain)
            bx_ignore_state_dispose_chain(top->owned_ignore_chain);
        state->dir_frame_len--;
    }
}

static const struct bx_ignore_state *
bx_search_walk_active_ignore_state(const struct bx_search_walk_state *state, int depth) {
    (void)depth;
    if (!state || state->dir_frame_len == 0u)
        return NULL;
    return state->dir_frames[state->dir_frame_len - 1u].ignore_state;
}

static uint64_t bx_search_walk_monotonic_ns(void) {
    struct timespec ts;
    uint64_t nanoseconds = 0;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0u;
    if (!bx_time_timespec_to_nanoseconds_u64(&ts, &nanoseconds))
        return 0u;
    return nanoseconds;
}

static uint64_t bx_search_walk_timing_start(void) {
    return bx_search_dev_counters_enabled() ? bx_search_walk_monotonic_ns() : 0u;
}

static void bx_search_walk_note_elapsed(enum bx_search_walk_counter counter,
                                        uint64_t start_ns) {
    if (start_ns == 0u)
        return;
    uint64_t end_ns = bx_search_walk_monotonic_ns();
    if (end_ns >= start_ns)
        bx_search_dev_counters_note_walk(counter, end_ns - start_ns);
}

static void bx_search_walk_note_rejected_entry(const struct bx_walk_entry *entry) {
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_REJECTED_ENTRIES, 1u);
    if (entry && entry->is_dir)
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_FILTER_REJECTED_DIRS, 1u);
}

static bool bx_search_walk_init_root_dir_frame(struct bx_search_walk_state *state,
                                               struct bx_ignore_state *owned_parent_ignore_state,
                                               const struct bx_ignore_state *borrowed_parent_ignore_state) {
    const struct bx_ignore_state *parent_ignore_state =
        owned_parent_ignore_state ? owned_parent_ignore_state : borrowed_parent_ignore_state;

    if (!state || !state->have_ignore_opts)
        return true;
    if (!parent_ignore_state)
        return true;
    if (!bx_search_walk_dir_frames_reserve(state, 1u))
        return false;

    state->dir_frames[0] = (struct bx_search_walk_dir_frame){
        .applies_depth = 0,
        .owned_ignore_chain = owned_parent_ignore_state,
        .ignore_state = parent_ignore_state,
    };
    state->dir_frame_len = 1u;
    return true;
}

static enum bx_walk_action bx_search_walk_push_dir_frame(struct bx_search_walk_state *state,
                                                         struct bx_walk_entry *entry,
                                                         int depth) {
    const struct bx_ignore_state *parent = bx_search_walk_active_ignore_state(state, depth);
    struct bx_ignore_program *program = NULL;

    if (state->have_ignore_opts && !state->ignore_opts.no_ignore) {
        program = bx_ignore_load_program(entry->path, &state->ignore_opts);
    }

    if (!program) {
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_STATE_FAST_PATHS, 1u);
        return BX_WALK_CONTINUE;
    }

    if (!bx_search_walk_dir_frames_reserve(state, state->dir_frame_len + 1u)) {
        bx_ignore_program_release(program);
        return BX_WALK_ERROR;
    }
    parent = bx_search_walk_active_ignore_state(state, depth);

    struct bx_search_walk_dir_frame *frame = &state->dir_frames[state->dir_frame_len];
    memset(frame, 0, sizeof(*frame));
    frame->applies_depth = depth + 1;
    bx_ignore_state_init(&frame->inline_ignore_state, parent, entry->path, program);
    frame->inline_ignore_state.owned_dirpath = strdup(entry->path);
    if (!frame->inline_ignore_state.owned_dirpath) {
        bx_ignore_state_dispose(&frame->inline_ignore_state);
        memset(frame, 0, sizeof(*frame));
        return BX_WALK_ERROR;
    }
    frame->inline_ignore_state.dirpath = frame->inline_ignore_state.owned_dirpath;
    frame->inline_ignore_state.dirpath_len = strlen(frame->inline_ignore_state.owned_dirpath);
    frame->ignore_state = &frame->inline_ignore_state;
    frame->have_inline_ignore_state = true;
    state->dir_frame_len++;
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_STATE_INLINE_FRAMES, 1u);
    return BX_WALK_CONTINUE;
}

static enum bx_walk_action bx_search_walk_visit(struct bx_walk_entry *entry, void *user) {
    struct bx_search_walk_state *state = user;
    bx_search_walk_dir_frames_pop_to_depth(state, entry->depth);

    uint64_t filter_start = entry->depth > 0 ? bx_search_walk_timing_start() : 0u;
    const char *name = bx_path_basename_ptr(entry->path);
    const struct bx_ignore_state *ignore_state =
        bx_search_walk_active_ignore_state(state, entry->depth);
    bool entry_selected = true;

    if (entry->depth > 0 &&
        bx_walk_filter_should_skip(&state->filter_state, entry, ignore_state, &entry_selected)) {
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_FILTER_NS, filter_start);
        bx_search_walk_note_rejected_entry(entry);
        return entry->is_dir ? BX_WALK_PRUNE : BX_WALK_CONTINUE;
    }

    if (entry_selected && entry->depth > 0 &&
        state->config->filter_opts &&
        state->config->filter_opts->num_include_patterns > 0 &&
        !entry->is_dir &&
        !bx_walk_filter_matches_include(&state->filter_state, name, entry->path)) {
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_FILTER_NS, filter_start);
        bx_search_walk_note_rejected_entry(entry);
        return BX_WALK_CONTINUE;
    }

    enum bx_walk_action action = BX_WALK_CONTINUE;
    if (!entry_selected) {
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_FILTER_NS, filter_start);
        bx_search_walk_note_rejected_entry(entry);
        action = BX_WALK_CONTINUE;
    } else if (state->config->visit_with_ignore) {
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_FILTER_NS, filter_start);
        action = state->config->visit_with_ignore(entry,
                                                  ignore_state,
                                                  state->have_ignore_opts ? &state->ignore_opts : NULL,
                                                  state->user);
    } else {
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_FILTER_NS, filter_start);
        action = state->config->visit(entry, state->user);
    }
    if (action == BX_WALK_CONTINUE && entry->prune)
        action = BX_WALK_PRUNE;

    if (state->have_ignore_opts && entry->is_dir && action == BX_WALK_CONTINUE) {
        uint64_t ignore_state_start = bx_search_walk_timing_start();
        enum bx_walk_action push_action =
            bx_search_walk_push_dir_frame(state, entry, entry->depth);
        bx_search_walk_note_elapsed(BX_SEARCH_WALK_IGNORE_STATE_NS, ignore_state_start);
        if (push_action != BX_WALK_CONTINUE)
            return push_action;
        if (state->dir_frame_len > 0u &&
            state->dir_frames[state->dir_frame_len - 1u].applies_depth == entry->depth + 1)
            bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_STATE_PUSHES, 1u);
    }

    return action;
}

static enum bx_walk_action bx_search_walk_error(const char *path, int errnum, void *user) {
    struct bx_search_walk_state *state = user;
    return state->config->error(path, errnum, state->user);
}

static int bx_search_walk_impl(const char *root,
                               DIR *root_dir,
                               const struct bx_search_walk_config *config,
                               void *user) {
    if (!root || !config || !config->walk_opts ||
        (!config->visit && !config->visit_with_ignore)) {
        if (root_dir)
            closedir(root_dir);
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
                if (root_dir)
                    closedir(root_dir);
                free(state.dir_frames);
                return -1;
            }
            state.ignore_opts.git_root = state.git_root_owned;
            state.ignore_opts.git_root_resolved = true;
        } else if (!state.ignore_opts.git_root_resolved) {
            state.git_root_owned = bx_ignore_find_git_root(root, &state.ignore_opts);
            state.ignore_opts.git_root = state.git_root_owned;
            state.ignore_opts.gitignore_enabled =
                state.git_root_owned != NULL ||
                (!state.ignore_opts.no_ignore &&
                 !state.ignore_opts.no_ignore_vcs &&
                 state.ignore_opts.no_require_git);
            state.ignore_opts.git_root_resolved = true;
        }

        if (config->borrowed_parent_ignore_state) {
            if (!bx_search_walk_init_root_dir_frame(&state,
                                                    NULL,
                                                    config->borrowed_parent_ignore_state)) {
                if (root_dir)
                    closedir(root_dir);
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
                if (root_dir)
                    closedir(root_dir);
                free(state.git_root_owned);
                free(state.dir_frames);
                return -1;
            }
            if (!bx_search_walk_init_root_dir_frame(&state, parent_ignore_state, NULL)) {
                bx_ignore_state_dispose_chain(parent_ignore_state);
                if (root_dir)
                    closedir(root_dir);
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
    int rc = root_dir
        ? bx_walk_opened_dir(root, root_dir, config->walk_opts, &ops, &state)
        : bx_walk(root, config->walk_opts, &ops, &state);

    bx_search_walk_dir_frames_pop_to_depth(&state, -1);
    free(state.git_root_owned);
    free(state.dir_frames);
    return rc;
}

int bx_search_walk(const char *root,
                   const struct bx_search_walk_config *config,
                   void *user) {
    return bx_search_walk_impl(root, NULL, config, user);
}

int bx_search_walk_opened_dir(const char *root,
                              DIR *root_dir,
                              const struct bx_search_walk_config *config,
                              void *user) {
    return bx_search_walk_impl(root, root_dir, config, user);
}
