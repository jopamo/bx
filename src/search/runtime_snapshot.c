#include "runtime_snapshot.h"

#include <string.h>
#include <stdlib.h>

#include "bx/libbx.h"
#include "options.h"
#include "search_internal.h"

struct bx_search_runtime_snapshot {
    char *progname;
    struct bx_walk_opts walk_opts;
    struct bx_walk_filter_opts filter_opts;
    struct bx_walk_ignore_opts ignore_opts;
    struct bx_search_pattern_vec include_patterns;
    struct bx_search_pattern_vec exclude_patterns;
    struct bx_search_pattern_vec exclude_dirs;
    char *extra_ignore_files[MAX_RG_IGNORE_FILES];
};

static void bx_search_runtime_snapshot_copy_string_array(char **dest,
                                                         char *const *src,
                                                         int count,
                                                         int max_count) {
    if (!dest || !src || count <= 0)
        return;
    if (count > max_count)
        count = max_count;
    for (int i = 0; i < count; ++i)
        dest[i] = src[i] ? xstrdup(src[i]) : NULL;
}

static void bx_search_runtime_snapshot_free_string_array(char **items,
                                                         int count,
                                                         int max_count) {
    if (!items || count <= 0)
        return;
    if (count > max_count)
        count = max_count;
    for (int i = 0; i < count; ++i)
        free(items[i]);
}

struct bx_search_runtime_snapshot *bx_search_runtime_snapshot_create(
    const char *progname,
    enum bx_search_personality personality,
    const struct search_opts *opts) {
    if (!opts)
        return NULL;

    struct bx_search_runtime_snapshot *snapshot = xmalloc(sizeof(*snapshot));
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->progname = xstrdup(progname ? progname : "grep");
    snapshot->walk_opts = bx_search_make_walk_opts(snapshot->progname,
                                                   personality,
                                                   opts,
                                                   NULL);
    snapshot->filter_opts = bx_search_make_filter_opts(opts);
    snapshot->ignore_opts = bx_search_make_ignore_opts(snapshot->progname, opts);

    if (!bx_search_pattern_vec_clone(&snapshot->include_patterns,
                                     &opts->include_patterns) ||
        !bx_search_pattern_vec_clone(&snapshot->exclude_patterns,
                                     &opts->exclude_patterns) ||
        !bx_search_pattern_vec_clone(&snapshot->exclude_dirs,
                                     &opts->exclude_dir_patterns)) {
        bx_search_runtime_snapshot_destroy(snapshot);
        return NULL;
    }
    snapshot->filter_opts.include_patterns = snapshot->include_patterns.items;
    snapshot->filter_opts.include_pattern_casefold =
        snapshot->include_patterns.casefold;
    snapshot->filter_opts.include_pattern_is_type =
        snapshot->include_patterns.is_type;
    snapshot->filter_opts.exclude_patterns = snapshot->exclude_patterns.items;
    snapshot->filter_opts.exclude_pattern_is_type =
        snapshot->exclude_patterns.is_type;
    snapshot->filter_opts.exclude_dirs = snapshot->exclude_dirs.items;

    bx_search_runtime_snapshot_copy_string_array(snapshot->extra_ignore_files,
                                                 opts->ignore_files,
                                                 opts->num_ignore_files,
                                                 MAX_RG_IGNORE_FILES);
    snapshot->ignore_opts.extra_ignore_files = snapshot->extra_ignore_files;
    return snapshot;
}

void bx_search_runtime_snapshot_destroy(struct bx_search_runtime_snapshot *snapshot) {
    if (!snapshot)
        return;

    bx_search_pattern_vec_dispose(&snapshot->include_patterns);
    bx_search_pattern_vec_dispose(&snapshot->exclude_patterns);
    bx_search_pattern_vec_dispose(&snapshot->exclude_dirs);
    bx_search_runtime_snapshot_free_string_array(snapshot->extra_ignore_files,
                                                 snapshot->ignore_opts.num_extra_ignore_files,
                                                 MAX_RG_IGNORE_FILES);
    free(snapshot->progname);
    free(snapshot);
}

const struct bx_walk_filter_opts *bx_search_runtime_snapshot_filter_opts(
    const struct bx_search_runtime_snapshot *snapshot) {
    return snapshot ? &snapshot->filter_opts : NULL;
}

const struct bx_walk_ignore_opts *bx_search_runtime_snapshot_ignore_opts(
    const struct bx_search_runtime_snapshot *snapshot) {
    return snapshot ? &snapshot->ignore_opts : NULL;
}

struct bx_walk_opts bx_search_runtime_snapshot_walk_opts(
    const struct bx_search_runtime_snapshot *snapshot,
    bool *stop) {
    struct bx_walk_opts walk_opts = {0};
    if (snapshot)
        walk_opts = snapshot->walk_opts;
    walk_opts.stop = stop;
    return walk_opts;
}

struct bx_walk_opts bx_search_runtime_snapshot_walk_opts_with_max_depth(
    const struct bx_search_runtime_snapshot *snapshot,
    bool *stop,
    int max_depth) {
    struct bx_walk_opts walk_opts =
        bx_search_runtime_snapshot_walk_opts(snapshot, stop);
    walk_opts.max_depth = max_depth;
    return walk_opts;
}

struct bx_walk_ignore_opts bx_search_runtime_snapshot_ignore_opts_with_git_root(
    const struct bx_search_runtime_snapshot *snapshot,
    const char *git_root,
    bool git_root_resolved,
    bool gitignore_enabled) {
    struct bx_walk_ignore_opts ignore_opts = {0};
    if (snapshot)
        ignore_opts = snapshot->ignore_opts;
    ignore_opts.git_root = git_root;
    ignore_opts.git_root_resolved = git_root_resolved;
    ignore_opts.gitignore_enabled = gitignore_enabled;
    return ignore_opts;
}
