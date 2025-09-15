#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ignore.h"
#include <sys/stat.h>
#include "walk.h"

struct walk_ancestor {
    dev_t dev;
    ino_t ino;
    const char *path;
    const struct walk_ancestor *parent;
};

static void walk_entry_fill_from_stat(struct walk_entry *entry, const struct stat *st) {
    entry->is_dir = S_ISDIR(st->st_mode);
    entry->metadata_loaded = true;
    entry->metadata_tried = true;
    entry->dev = st->st_dev;
    entry->mode = st->st_mode;
    entry->inode = st->st_ino;
    entry->nlink = st->st_nlink;
    entry->uid = st->st_uid;
    entry->gid = st->st_gid;
    entry->size = st->st_size;
    entry->atime = st->st_atim;
    entry->mtime = st->st_mtim;
    entry->ctime = st->st_ctim;
}

bool walk_entry_load_metadata(struct walk_entry *entry) {
    if (!entry)
        return false;
    if (entry->metadata_loaded)
        return true;
    if (entry->metadata_tried)
        return false;

    entry->metadata_tried = true;

    struct stat st;
    int rc = entry->follow_metadata ? stat(entry->path, &st) : lstat(entry->path, &st);
    if (rc != 0)
        return false;

    walk_entry_fill_from_stat(entry, &st);
    return true;
}

static bool walk_should_stop(const struct walk_opts *opts) {
    return opts->stop && *opts->stop;
}

static const char *walk_error_prefix(const struct walk_opts *opts) {
    return (opts && opts->error_prefix) ? opts->error_prefix : "walk";
}

static void walk_report_error(const struct walk_opts *opts, const char *path, int errnum) {
    if (opts && opts->os_error_style)
        fprintf(stderr, "%s: %s: %s (os error %d)\n",
                walk_error_prefix(opts), path, strerror(errnum), errnum);
    else
        fprintf(stderr, "%s: %s: %s\n",
                walk_error_prefix(opts), path, strerror(errnum));
}

static void walk_report_loop(const struct walk_opts *opts, const char *path) {
    if (!opts || opts->cycle_report == WALK_CYCLE_IGNORE)
        return;

    if (opts->os_error_style) {
        walk_report_error(opts, path, ELOOP);
        return;
    }

    if (opts->cycle_report == WALK_CYCLE_WARN) {
        fprintf(stderr, "%s: %s: warning: recursive directory loop\n",
                walk_error_prefix(opts), path);
        return;
    }

    fprintf(stderr, "%s: %s: file system loop detected\n",
            walk_error_prefix(opts), path);
}

static bool walk_ancestor_contains(const struct walk_ancestor *anc, dev_t dev, ino_t ino) {
    for (const struct walk_ancestor *it = anc; it; it = it->parent) {
        if (it->dev == dev && it->ino == ino)
            return true;
    }
    return false;
}

static bool is_hidden(const char *name) {
    return name[0] == '.';
}

static const char *walk_relative_path(const char *root, const char *path) {
    if (!root || !path)
        return path;

    size_t root_len = strlen(root);
    if (strncmp(path, root, root_len) != 0)
        return path;
    if (path[root_len] == '/')
        return path + root_len + 1;
    if (path[root_len] == '\0')
        return path + root_len;
    return path;
}

static bool walk_matches_exclude_pattern(const struct walk_opts *opts,
                                         const char *name,
                                         const char *relative_path) {
    if (!opts || !opts->exclude_patterns || opts->num_exclude_patterns <= 0)
        return false;

    for (int i = 0; i < opts->num_exclude_patterns; i++) {
        const char *pattern = opts->exclude_patterns[i];
        if (!pattern || pattern[0] == '\0')
            continue;
        if (fnmatch(pattern, name, 0) == 0)
            return true;
        if (relative_path && relative_path[0] != '\0' &&
            fnmatch(pattern, relative_path, FNM_PATHNAME) == 0)
            return true;
    }

    return false;
}

static bool walk_matches_include_pattern(const struct walk_opts *opts,
                                         const char *name,
                                         const char *relative_path) {
    if (!opts || !opts->include_patterns || opts->num_include_patterns <= 0)
        return false;

    for (int i = 0; i < opts->num_include_patterns; i++) {
        const char *pattern = opts->include_patterns[i];
        int flags = 0;
        if (!pattern || pattern[0] == '\0')
            continue;
        if (opts->include_pattern_casefold && opts->include_pattern_casefold[i])
            flags |= FNM_CASEFOLD;
        if (fnmatch(pattern, name, flags) == 0)
            return true;
        if (relative_path && relative_path[0] != '\0' &&
            fnmatch(pattern, relative_path, FNM_PATHNAME | flags) == 0)
            return true;
    }

    return false;
}

static int walk_recursive(const char *dirpath, struct walk_opts *opts,
                          walk_callback cb, void *user, int depth,
                          const struct walk_ancestor *ancestors,
                          char **parent_ignore_patterns, int parent_ignore_n,
                          const char *root_path) {
    if (walk_should_stop(opts))
        return 0;

    if (opts->max_depth >= 0 && depth > opts->max_depth)
        return 0;

    char **local_ignore_patterns = NULL;
    int local_ignore_n = 0;
    if (!opts->no_ignore)
        bx_ignore_load_patterns(dirpath, opts, &local_ignore_patterns, &local_ignore_n);

    char **ignore_patterns = NULL;
    int ignore_n = 0;
    int ignore_cap = 0;
    if (!bx_ignore_clone_patterns(parent_ignore_patterns, parent_ignore_n,
                                  &ignore_patterns, &ignore_n, &ignore_cap)) {
        bx_ignore_free_patterns(local_ignore_patterns, local_ignore_n);
        return -1;
    }
    for (int i = 0; i < local_ignore_n; i++) {
        if (!bx_ignore_append_pattern(&ignore_patterns, &ignore_n, &ignore_cap,
                                      local_ignore_patterns[i])) {
            bx_ignore_free_patterns(local_ignore_patterns, local_ignore_n);
            bx_ignore_free_patterns(ignore_patterns, ignore_n);
            return -1;
        }
    }
    bx_ignore_free_patterns(local_ignore_patterns, local_ignore_n);

    DIR *d = opendir(dirpath);
    if (!d) {
        if (errno == EACCES && opts->suppress_eacces) {
            if (opts->report_eacces)
                walk_report_error(opts, dirpath, errno);
            bx_ignore_free_patterns(ignore_patterns, ignore_n);
            return 0;
        }
        walk_report_error(opts, dirpath, errno);
        bx_ignore_free_patterns(ignore_patterns, ignore_n);
        return -1;
    }

    int status = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (walk_should_stop(opts))
            break;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        size_t plen = strlen(dirpath) + 1 + strlen(ent->d_name) + 1;
        char *full = malloc(plen);
        snprintf(full, plen, "%s/%s", dirpath, ent->d_name);

        const char *relative_path = walk_relative_path(root_path, full);

        if (!opts->hidden && is_hidden(ent->d_name) &&
            !walk_matches_include_pattern(opts, ent->d_name, relative_path)) {
            free(full);
            continue;
        }

        if (bx_ignore_path_ignored(ent->d_name, ignore_patterns, ignore_n)) {
            free(full);
            continue;
        }

        if (opts->exclude_dirs) {
            bool skip = false;
            for (int e = 0; e < opts->num_exclude_dirs; e++)
                if (fnmatch(opts->exclude_dirs[e], ent->d_name, 0) == 0) skip = true;
            if (skip) {
                free(full);
                continue;
            }
        }

        if (walk_matches_exclude_pattern(opts, ent->d_name, relative_path)) {
            free(full);
            continue;
        }

        if (opts->max_depth >= 0 && depth + 1 > opts->max_depth) {
            free(full);
            continue;
        }

        struct stat st;
        struct stat lst;
        bool entry_was_symlink = false;
        struct walk_entry entry = {
            .path = full,
            .follow_metadata = opts->follow_symlinks,
            .depth = depth + 1,
        };

        if (!opts->follow_symlinks) {
            if (ent->d_type == DT_DIR) {
                entry.is_dir = true;
            } else if (ent->d_type != DT_UNKNOWN) {
                entry.is_dir = false;
            } else {
                if (lstat(full, &st) != 0) {
                    free(full);
                    continue;
                }
                walk_entry_fill_from_stat(&entry, &st);
            }
        } else {
            if (ent->d_type == DT_DIR) {
                entry.is_dir = true;
            } else if (ent->d_type != DT_LNK && ent->d_type != DT_UNKNOWN) {
                entry.is_dir = false;
            } else {
                if (lstat(full, &lst) != 0) {
                    free(full);
                    continue;
                }
                entry_was_symlink = S_ISLNK(lst.st_mode);
                if (stat(full, &st) != 0) {
                    free(full);
                    continue;
                }
                walk_entry_fill_from_stat(&entry, &st);
            }
        }

        if (!opts->post_order || !entry.is_dir)
            cb(&entry, user);

        if (!walk_should_stop(opts) && entry.is_dir) {
            if (!walk_entry_load_metadata(&entry)) {
                free(full);
                continue;
            }

            bool repeated_dir = false;
            if (opts->cycle_mode == WALK_CYCLE_DIR_REPEAT) {
                repeated_dir = walk_ancestor_contains(ancestors, entry.dev, entry.inode);
            } else if (opts->cycle_mode == WALK_CYCLE_SYMLINK_REPEAT) {
                repeated_dir = entry_was_symlink &&
                               walk_ancestor_contains(ancestors, entry.dev, entry.inode);
            }

            if (repeated_dir) {
                walk_report_loop(opts, full);
                if (opts->cycle_report == WALK_CYCLE_ERROR)
                    status = -1;
            } else {
                struct walk_ancestor next = {
                    .dev = entry.dev,
                    .ino = entry.inode,
                    .path = full,
                    .parent = ancestors,
                };
                if (walk_recursive(full, opts, cb, user, depth + 1, &next,
                                   ignore_patterns, ignore_n, root_path) != 0)
                    status = -1;
            }
        }
        if (!walk_should_stop(opts) && opts->post_order && entry.is_dir)
            cb(&entry, user);
        free(full);
    }
    closedir(d);
    bx_ignore_free_patterns(ignore_patterns, ignore_n);
    return status;
}

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user) {
    struct walk_opts effective_opts = *opts;
    effective_opts.gitignore_enabled = bx_ignore_enable_gitignore_for_root(root, opts);

    struct stat st;
    int root_stat_rc = (opts && opts->follow_root_symlink) ? stat(root, &st) : lstat(root, &st);
    if (root_stat_rc != 0) {
        walk_report_error(opts, root, errno);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        struct walk_entry entry = {
            .path = strdup(root),
            .follow_metadata = opts && opts->follow_root_symlink,
            .depth = 0,
        };
        walk_entry_fill_from_stat(&entry, &st);
        if (!opts->post_order)
            cb(&entry, user);
        free(entry.path);
        if (walk_should_stop(opts))
            return 0;
        struct walk_ancestor root_ancestor = {
            .dev = st.st_dev,
            .ino = st.st_ino,
            .path = root,
            .parent = NULL,
        };
        char **parent_ignore_patterns = NULL;
        int parent_ignore_n = 0;
        if (!bx_ignore_load_parent_patterns(root, &effective_opts,
                                            &parent_ignore_patterns, &parent_ignore_n))
            return -1;
        int rc = walk_recursive(root, &effective_opts, cb, user, 0, &root_ancestor,
                                parent_ignore_patterns, parent_ignore_n, root);
        bx_ignore_free_patterns(parent_ignore_patterns, parent_ignore_n);
        if (!walk_should_stop(opts) && opts->post_order) {
            struct walk_entry post = {
                .path = strdup(root),
                .follow_metadata = opts && opts->follow_root_symlink,
                .depth = 0,
            };
            walk_entry_fill_from_stat(&post, &st);
            cb(&post, user);
            free(post.path);
        }
        return rc;
    }

    struct walk_entry entry = {
        .path = strdup(root),
        .follow_metadata = opts && opts->follow_root_symlink,
        .depth = 0,
    };
    walk_entry_fill_from_stat(&entry, &st);
    cb(&entry, user);
    free(entry.path);
    return 0;
}
