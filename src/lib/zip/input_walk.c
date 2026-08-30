#define _XOPEN_SOURCE 700

#include "input_walk.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fileio.h"
#include "ziputils.h"
#include "lib/path_ops.h"

#ifndef FNM_CASEFOLD
#define FNM_CASEFOLD 0
#endif

static const char* strip_leading_dot_slash(const char* path) {
    const char* cursor = path ? path : "";
    while (cursor[0] == '.' && cursor[1] == '/')
        cursor = bx_path_strip_dot_slash_prefix_ptr(cursor);
    return bx_path_is_dot_or_dotdot(cursor) && cursor[1] == '\0'
        ? cursor + 1
        : cursor;
}

/*
 * Recursively walk a directory and add file operands into list
 *
 * Behavior
 * - Uses opendir/readdir and lstat to avoid following symlinks by accident
 * - Adds each regular file path to list
 * - When allow_symlinks is true, also adds symlink paths as operands
 *
 * Directory entries
 * - If store_paths is enabled and no_dir_entries is false, an explicit directory entry with
 *   a trailing '/' is also added for each directory visited (except "." which normalizes empty)
 *
 * Error policy
 * - Failure to open or stat subpaths is logged as a warning and traversal continues
 * - Allocation failure is treated as fatal and returns ZU_STATUS_OOM
 */
static int walk_dir(ZContext* ctx, const char* root, ZU_StrList* list) {
    DIR* d = opendir(root);
    if (!d) {
        zu_log(ctx, "warning: could not open directory %s: %s\n", root, strerror(errno));
        return ZU_STATUS_OK;
    }

    if (!ctx->no_dir_entries && ctx->store_paths) {
        const char* normalized = strip_leading_dot_slash(root);
        if (normalized[0] != '\0') {
            size_t len = strlen(normalized);
            char* dir_entry = malloc(len + 2u);
            if (!dir_entry) {
                closedir(d);
                return ZU_STATUS_OOM;
            }
            memcpy(dir_entry, normalized, len);
            dir_entry[len] = '/';
            dir_entry[len + 1u] = '\0';
            int rc = zu_strlist_push(list, dir_entry);
            free(dir_entry);
            if (rc != 0) {
                closedir(d);
                return ZU_STATUS_OOM;
            }
        }
    }

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char* path = bx_path_join(root, entry->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) {
            zu_log(ctx, "warning: could not stat %s: %s\n", path, strerror(errno));
            free(path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc = walk_dir(ctx, path, list);
            free(path);
            if (rc != ZU_STATUS_OK) {
                closedir(d);
                return rc;
            }
        }
        else if (S_ISREG(st.st_mode) || (ctx->allow_symlinks && S_ISLNK(st.st_mode))) {
            const char* normalized = strip_leading_dot_slash(path);
            if (zu_strlist_push(list, normalized) != 0) {
                free(path);
                closedir(d);
                return ZU_STATUS_OOM;
            }
            free(path);
        }
        else {
            free(path);
        }
    }

    closedir(d);
    return ZU_STATUS_OK;
}

/*
 * Check whether a path matches any of the provided patterns
 *
 * Pattern semantics
 * - Uses fnmatch
 * - If match_case is false and FNM_CASEFOLD is available, enable case-folded matching
 *
 * Empty pattern list
 * - Treated as match-all to keep callers simple
 */
static bool matches_any_pattern(const ZU_StrList* patterns, const char* path, bool match_case) {
    if (!patterns || patterns->len == 0)
        return true;

    int flags = match_case ? 0 : FNM_CASEFOLD;

    for (size_t i = 0; i < patterns->len; ++i) {
        if (fnmatch(patterns->items[i], path, flags) == 0)
            return true;
    }

    return false;
}

/*
 * Walk the filesystem from root and collect files that match operand patterns
 *
 * This is used for PKZIP-style recursion from current directory (-R)
 * - patterns is the initial list of user-provided operands (ctx->include)
 * - Every discovered file is filtered by patterns and then by include/exclude rules
 *
 * Error policy
 * - Directory open failures are ignored to keep behavior closer to zip tools
 * - Allocation failures are fatal
 */
static int walk_dir_patterns(ZContext* ctx, const char* root, ZU_StrList* out, const ZU_StrList* patterns) {
    DIR* d = opendir(root);
    if (!d)
        return ZU_STATUS_OK;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char* path = (strcmp(root, ".") == 0 || root[0] == '\0')
            ? strdup(entry->d_name)
            : bx_path_join(root, entry->d_name);
        if (!path) {
            closedir(d);
            return ZU_STATUS_OOM;
        }

        struct stat st;
        if (lstat(path, &st) != 0) {
            free(path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            int rc = walk_dir_patterns(ctx, path, out, patterns);
            free(path);
            if (rc != ZU_STATUS_OK) {
                closedir(d);
                return rc;
            }
            continue;
        }

        // Accept regular files, and optionally symlinks and FIFOs depending on policy flags
        if (!(S_ISREG(st.st_mode) || (ctx->allow_symlinks && S_ISLNK(st.st_mode)) || (ctx->allow_fifo && S_ISFIFO(st.st_mode)))) {
            free(path);
            continue;
        }

        const char* normalized = strip_leading_dot_slash(path);

        if (!matches_any_pattern(patterns, normalized, ctx->match_case)) {
            free(path);
            continue;
        }

        if (!zu_should_include(ctx, normalized)) {
            free(path);
            continue;
        }

        if (zu_strlist_push(out, normalized) != 0) {
            free(path);
            closedir(d);
            return ZU_STATUS_OOM;
        }
        free(path);
    }

    closedir(d);
    return ZU_STATUS_OK;
}

/*
 * Decide whether a candidate path should be included based on ctx patterns
 *
 * Precedence rules
 * - Exclude patterns win immediately
 * - If no include_patterns are set, everything not excluded is included
 * - Otherwise the path must match at least one include_pattern
 *
 * Notes
 * - This is separate from operand matching
 *   operand matching selects candidates, include/exclude further filters them
 */
bool zu_should_include(const ZContext* ctx, const char* name) {
    int flags = ctx->match_case ? 0 : FNM_CASEFOLD;

    for (size_t i = 0; i < ctx->exclude.len; ++i) {
        if (fnmatch(ctx->exclude.items[i], name, flags) == 0) {
            return false;
        }
    }

    if (ctx->include_patterns.len == 0) {
        return true;
    }

    for (size_t i = 0; i < ctx->include_patterns.len; ++i) {
        if (fnmatch(ctx->include_patterns.items[i], name, flags) == 0) {
            return true;
        }
    }

    return false;
}

/*
 * Expand ctx->include operands when recursion is enabled
 *
 * Modes
 * - recursive + recurse_from_cwd (-R)
 *   Walk from ".", collecting any files matching the operand patterns in ctx->include
 *   The resulting collected list replaces ctx->include
 *
 * - recursive (-r)
 *   For each operand in ctx->include:
 *   - If it is a directory, recursively walk it and collect file paths
 *   - Otherwise, keep it as-is
 *   After collection, apply include/exclude rules to produce the final ctx->include list
 *
 * Return values
 * - ZU_STATUS_OK on success
 * - ZU_STATUS_OOM on allocation failure
 * - ZU_STATUS_USAGE if ctx is NULL
 */
int zu_expand_args(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    if (ctx->recursive && ctx->recurse_from_cwd) {
        ZU_StrList collected;
        zu_strlist_init(&collected);

        int rc = walk_dir_patterns(ctx, ".", &collected, &ctx->include);
        if (rc != ZU_STATUS_OK) {
            zu_strlist_free(&collected);
            return rc;
        }

        zu_strlist_free(&ctx->include);
        ctx->include = collected;
        return ZU_STATUS_OK;
    }

    if (!ctx->recursive) {
        return ZU_STATUS_OK;
    }

    ZU_StrList new_list;
    zu_strlist_init(&new_list);

    for (size_t i = 0; i < ctx->include.len; ++i) {
        const char* path = ctx->include.items[i];

        struct stat st;
        if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            int rc = walk_dir(ctx, path, &new_list);
            if (rc != ZU_STATUS_OK) {
                zu_strlist_free(&new_list);
                return rc;
            }
        }
        else {
            const char* normalized = strip_leading_dot_slash(path);
            if (*normalized != '\0')
                zu_strlist_push(&new_list, normalized);
        }
    }

    // Filter collected paths through include/exclude lists
    ZU_StrList filtered;
    zu_strlist_init(&filtered);

    for (size_t i = 0; i < new_list.len; ++i) {
        const char* path = new_list.items[i];
        if (!zu_should_include(ctx, path))
            continue;

        if (zu_strlist_push(&filtered, path) != 0) {
            zu_strlist_free(&new_list);
            zu_strlist_free(&filtered);
            return ZU_STATUS_OOM;
        }
    }

    zu_strlist_free(&new_list);

    // Replace ctx->include with the filtered list
    zu_strlist_free(&ctx->include);
    ctx->include = filtered;

    return ZU_STATUS_OK;
}
