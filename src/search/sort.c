#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "lib/statx_compat.h"
#include "search_internal.h"
#include "sort.h"
#include "traverse.h"

enum bx_search_sort_add_result {
    BX_SEARCH_SORT_ADD_CONTINUE = 0,
    BX_SEARCH_SORT_ADD_STOP,
    BX_SEARCH_SORT_ADD_ERROR,
};

struct bx_search_sort_collect_state {
    const char *progname;
    const struct search_opts *opts;
    struct bx_search_sorted_paths *out;
    bool *error_seen;
    bool strip_dot_prefix;
    size_t next_sequence;
    bool fatal_sort_error;
};

static enum bx_search_sort_dir bx_search_sort_compare_dir = BX_SEARCH_SORT_ASCENDING;

bool bx_search_sort_requested(const struct search_opts *opts) {
    return opts && opts->sort_key != BX_SEARCH_SORT_NONE;
}

bool bx_search_sort_is_path(const struct search_opts *opts) {
    return opts && opts->sort_key == BX_SEARCH_SORT_PATH;
}

bool bx_search_sort_is_metadata(const struct search_opts *opts) {
    return opts && opts->sort_key != BX_SEARCH_SORT_NONE
        && opts->sort_key != BX_SEARCH_SORT_PATH;
}

bool bx_search_sort_is_descending(const struct search_opts *opts) {
    return opts && opts->sort_dir == BX_SEARCH_SORT_DESCENDING;
}

void bx_search_sorted_paths_dispose(struct bx_search_sorted_paths *paths) {
    if (!paths)
        return;
    for (size_t i = 0; i < paths->len; ++i)
        free(paths->items[i].path);
    free(paths->items);
    paths->items = NULL;
    paths->len = 0u;
    paths->cap = 0u;
}

static bool bx_search_sorted_paths_reserve(struct bx_search_sorted_paths *paths,
                                           size_t needed) {
    if (paths->cap >= needed)
        return true;

    size_t new_cap = paths->cap == 0u ? 16u : paths->cap * 2u;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2u)
            return false;
        new_cap *= 2u;
    }

    struct bx_search_sorted_path *tmp =
        realloc(paths->items, new_cap * sizeof(*paths->items));
    if (!tmp)
        return false;

    paths->items = tmp;
    paths->cap = new_cap;
    return true;
}

static int bx_search_sort_time_compare(const struct timespec *left,
                                       const struct timespec *right) {
    if (left->tv_sec != right->tv_sec)
        return left->tv_sec < right->tv_sec ? -1 : 1;
    if (left->tv_nsec != right->tv_nsec)
        return left->tv_nsec < right->tv_nsec ? -1 : 1;
    return 0;
}

static int bx_search_sorted_path_compare(const void *left, const void *right) {
    const struct bx_search_sorted_path *a = left;
    const struct bx_search_sorted_path *b = right;
    int cmp = bx_search_sort_time_compare(&a->sort_time, &b->sort_time);

    if (cmp != 0)
        return bx_search_sort_compare_dir == BX_SEARCH_SORT_DESCENDING ? -cmp : cmp;

    cmp = strcmp(a->path, b->path);
    if (cmp != 0)
        return cmp;

    return (a->sequence > b->sequence) - (a->sequence < b->sequence);
}

static int bx_search_sort_lookup_time(const char *path,
                                      const struct search_opts *opts,
                                      struct timespec *out,
                                      bool *available) {
    struct stat st;

    if (!path || !opts || !out || !available) {
        errno = EINVAL;
        return -1;
    }

    *available = true;
    switch (opts->sort_key) {
    case BX_SEARCH_SORT_MODIFIED:
        if (stat(path, &st) != 0)
            return -1;
        *out = st.st_mtim;
        return 0;
    case BX_SEARCH_SORT_ACCESSED:
        if (stat(path, &st) != 0)
            return -1;
        *out = st.st_atim;
        return 0;
    case BX_SEARCH_SORT_CREATED:
        return bx_statx_get_btime(path, out, available);
    case BX_SEARCH_SORT_NONE:
    case BX_SEARCH_SORT_PATH:
        errno = EINVAL;
        return -1;
    }

    errno = EINVAL;
    return -1;
}

static void bx_search_report_created_sort_unavailable(const char *progname,
                                                      const char *path,
                                                      const struct search_opts *opts) {
    fprintf(stderr,
            "%s: %s: creation time is unavailable; cannot use %s=created\n",
            progname,
            path,
            bx_search_sort_is_descending(opts) ? "--sortr" : "--sort");
}

static enum bx_search_sort_add_result bx_search_sorted_paths_add(
    struct bx_search_sort_collect_state *state,
    const char *path,
    bool strip_dot_prefix
) {
    struct timespec sort_time = {0};
    bool available = false;

    if (!state || !path)
        return BX_SEARCH_SORT_ADD_ERROR;

    if (bx_search_sort_lookup_time(path, state->opts, &sort_time, &available) != 0) {
        bx_search_report_path_error(state->progname, path, errno, state->opts);
        if (state->error_seen)
            *state->error_seen = true;
        return BX_SEARCH_SORT_ADD_CONTINUE;
    }

    if (!available) {
        bx_search_report_created_sort_unavailable(state->progname, path, state->opts);
        if (state->error_seen)
            *state->error_seen = true;
        state->fatal_sort_error = true;
        return BX_SEARCH_SORT_ADD_STOP;
    }

    if (!bx_search_sorted_paths_reserve(state->out, state->out->len + 1u))
        return BX_SEARCH_SORT_ADD_ERROR;

    char *owned_path = strdup(path);
    if (!owned_path)
        return BX_SEARCH_SORT_ADD_ERROR;

    state->out->items[state->out->len++] = (struct bx_search_sorted_path){
        .path = owned_path,
        .strip_dot_prefix = strip_dot_prefix,
        .sort_time = sort_time,
        .sequence = state->next_sequence++,
    };
    return BX_SEARCH_SORT_ADD_CONTINUE;
}

static enum bx_walk_action bx_search_sort_walk_cb(struct bx_walk_entry *entry, void *user) {
    struct bx_search_sort_collect_state *state = user;

    if (!state || !entry)
        return BX_WALK_ERROR;
    if (entry->is_dir)
        return BX_WALK_CONTINUE;
    if (bx_search_entry_exceeds_max_filesize(entry, state->opts))
        return BX_WALK_CONTINUE;
    if (bx_search_entry_should_skip_special_input(entry, state->opts))
        return BX_WALK_CONTINUE;

    switch (bx_search_sorted_paths_add(state, entry->path, state->strip_dot_prefix)) {
    case BX_SEARCH_SORT_ADD_CONTINUE:
        return BX_WALK_CONTINUE;
    case BX_SEARCH_SORT_ADD_STOP:
        return BX_WALK_STOP;
    case BX_SEARCH_SORT_ADD_ERROR:
        return BX_WALK_ERROR;
    }

    return BX_WALK_ERROR;
}

static enum bx_walk_action bx_search_sort_walk_error_cb(const char *path,
                                                        int errnum,
                                                        void *user) {
    struct bx_search_sort_collect_state *state = user;

    if (!state)
        return BX_WALK_ERROR;
    bx_search_report_path_error(state->progname, path, errnum, state->opts);
    if (state->error_seen)
        *state->error_seen = true;
    return BX_WALK_CONTINUE;
}

static int bx_search_collect_metadata_sorted_root(
    const char *root,
    bool strip_dot_prefix,
    const char *progname,
    enum bx_search_personality personality,
    const struct search_opts *opts,
    struct bx_search_sort_collect_state *state,
    bool *stop
) {
    struct bx_walk_opts walk_opts = bx_search_make_walk_opts(progname, personality, opts, stop);
    struct bx_walk_filter_opts filter_opts = bx_search_make_filter_opts(opts);
    struct bx_walk_ignore_opts ignore_opts = bx_search_make_ignore_opts(progname, opts);
    struct bx_search_walk_config walk_config = {
        .walk_opts = &walk_opts,
        .filter_opts = &filter_opts,
        .ignore_opts = &ignore_opts,
        .visit = bx_search_sort_walk_cb,
        .error = bx_search_sort_walk_error_cb,
    };

    state->strip_dot_prefix = strip_dot_prefix;
    return bx_search_walk(root, &walk_config, state);
}

int bx_search_collect_metadata_sorted_paths(int argc,
                                            char **argv,
                                            int first_file,
                                            const char *progname,
                                            enum bx_search_personality personality,
                                            const struct search_opts *opts,
                                            struct bx_search_sorted_paths *out,
                                            bool *error_seen) {
    if (!argv || !progname || !opts || !out || !bx_search_sort_is_metadata(opts)) {
        errno = EINVAL;
        return -1;
    }

    bool stop = false;
    struct bx_search_sort_collect_state state = {
        .progname = progname,
        .opts = opts,
        .out = out,
        .error_seen = error_seen,
    };
    int num_files = argc - first_file;

    if (error_seen)
        *error_seen = false;

    if (num_files == 0) {
        if (bx_search_collect_metadata_sorted_root(".", true, progname, personality, opts,
                                                   &state, &stop) != 0) {
            bx_search_sorted_paths_dispose(out);
            return -1;
        }
    } else if (opts->recursive) {
        for (int operand_i = 0; operand_i < num_files && !stop; ++operand_i) {
            int j = first_file + operand_i;
            struct stat st;

            if (stat(argv[j], &st) != 0) {
                bx_search_report_path_error(progname, argv[j], errno, opts);
                if (error_seen)
                    *error_seen = true;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                if (bx_search_collect_metadata_sorted_root(argv[j], false, progname,
                                                           personality, opts, &state,
                                                           &stop) != 0) {
                    bx_search_sorted_paths_dispose(out);
                    return -1;
                }
                continue;
            }
            if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                continue;
            if (!bx_search_explicit_entry_selected(opts, argv[j]))
                continue;
            if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                continue;

            switch (bx_search_sorted_paths_add(&state, argv[j], false)) {
            case BX_SEARCH_SORT_ADD_CONTINUE:
                break;
            case BX_SEARCH_SORT_ADD_STOP:
                stop = true;
                break;
            case BX_SEARCH_SORT_ADD_ERROR:
                bx_search_sorted_paths_dispose(out);
                return -1;
            }
        }
    } else {
        for (int operand_i = 0; operand_i < num_files; ++operand_i) {
            int j = first_file + operand_i;

            if (argv[j] && strcmp(argv[j], "-") != 0) {
                struct stat st;
                if (lstat(argv[j], &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        bx_search_report_path_error(progname, argv[j], EISDIR, opts);
                        if (error_seen)
                            *error_seen = true;
                        continue;
                    }
                    if (bx_search_should_skip_special_input_mode(st.st_mode, opts))
                        continue;
                }
                if (bx_search_path_exceeds_max_filesize(argv[j], opts))
                    continue;
            }

            switch (bx_search_sorted_paths_add(&state, argv[j], false)) {
            case BX_SEARCH_SORT_ADD_CONTINUE:
                break;
            case BX_SEARCH_SORT_ADD_STOP:
                stop = true;
                break;
            case BX_SEARCH_SORT_ADD_ERROR:
                bx_search_sorted_paths_dispose(out);
                return -1;
            }
            if (stop)
                break;
        }
    }

    if (state.fatal_sort_error) {
        bx_search_sorted_paths_dispose(out);
        return -1;
    }

    if (out->len > 1u) {
        bx_search_sort_compare_dir = opts->sort_dir;
        qsort(out->items, out->len, sizeof(*out->items), bx_search_sorted_path_compare);
    }
    return 0;
}
