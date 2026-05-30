#ifndef BX_SEARCH_RUNTIME_SNAPSHOT_H
#define BX_SEARCH_RUNTIME_SNAPSHOT_H

#include <stdbool.h>

#include "fswalk/walk.h"
#include "search.h"

struct search_opts;

/*
 * Immutable search traversal policy snapshot.
 *
 * Parsing owns mutable requested state in struct search_opts. Before recursive
 * traversal starts, callers publish traversal, filter, and ignore matcher
 * policy in this snapshot and workers borrow it read-only. Per-walk stop
 * pointers, relative max-depth, and donated-subtree git-root state are
 * returned as local copies so the published snapshot remains immutable.
 */
struct bx_search_runtime_snapshot;

struct bx_search_runtime_snapshot *bx_search_runtime_snapshot_create(
    const char *progname,
    enum bx_search_personality personality,
    const struct search_opts *opts);
void bx_search_runtime_snapshot_destroy(struct bx_search_runtime_snapshot *snapshot);

const struct bx_walk_filter_opts *bx_search_runtime_snapshot_filter_opts(
    const struct bx_search_runtime_snapshot *snapshot);
const struct bx_walk_ignore_opts *bx_search_runtime_snapshot_ignore_opts(
    const struct bx_search_runtime_snapshot *snapshot);

struct bx_walk_opts bx_search_runtime_snapshot_walk_opts(
    const struct bx_search_runtime_snapshot *snapshot,
    bool *stop);
struct bx_walk_opts bx_search_runtime_snapshot_walk_opts_with_max_depth(
    const struct bx_search_runtime_snapshot *snapshot,
    bool *stop,
    int max_depth);
struct bx_walk_ignore_opts bx_search_runtime_snapshot_ignore_opts_with_git_root(
    const struct bx_search_runtime_snapshot *snapshot,
    const char *git_root,
    bool git_root_resolved,
    bool gitignore_enabled);

#endif /* BX_SEARCH_RUNTIME_SNAPSHOT_H */
