#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "filter.h"
#include "ignore.h"
#include <sys/stat.h>
#include "walk.h"

struct walk_ancestor {
    dev_t dev;
    ino_t ino;
    const char *path;
    const struct walk_ancestor *parent;
};

struct walk_dirent_item {
    char *name;
    unsigned char d_type;
};

struct walk_dirent_list {
    struct walk_dirent_item *items;
    size_t len;
    size_t cap;
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
    entry->block_size = st->st_blksize;
    entry->atime = st->st_atim;
    entry->mtime = st->st_mtim;
    entry->ctime = st->st_ctim;
}

static void walk_dirent_list_free(struct walk_dirent_list *list) {
    if (!list)
        return;

    for (size_t i = 0; i < list->len; i++)
        free(list->items[i].name);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool walk_dirent_list_reserve(struct walk_dirent_list *list, size_t needed) {
    if (list->cap >= needed)
        return true;

    size_t new_cap = list->cap == 0 ? 16 : list->cap * 2;
    while (new_cap < needed)
        new_cap *= 2;

    struct walk_dirent_item *tmp = realloc(list->items, new_cap * sizeof(*list->items));
    if (!tmp)
        return false;

    list->items = tmp;
    list->cap = new_cap;
    return true;
}

static int walk_dirent_item_compare(const void *left, const void *right) {
    const struct walk_dirent_item *a = left;
    const struct walk_dirent_item *b = right;
    return strcmp(a->name, b->name);
}

static bool walk_dirent_list_read_sorted(DIR *dir, struct walk_dirent_list *list) {
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (!walk_dirent_list_reserve(list, list->len + 1))
            return false;

        list->items[list->len].name = strdup(ent->d_name);
        if (!list->items[list->len].name)
            return false;
        list->items[list->len].d_type = ent->d_type;
        list->len++;
    }

    if (list->len > 1)
        qsort(list->items, list->len, sizeof(*list->items), walk_dirent_item_compare);

    return true;
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

static int walk_recursive(const char *dirpath, struct walk_opts *opts,
                          walk_callback cb, void *user, int depth,
                          const struct walk_ancestor *ancestors,
                          struct bx_ignore_state *parent_ignore_state,
                          const struct bx_walk_filter_state *filters) {
    if (walk_should_stop(opts))
        return 0;

    if (opts->max_depth >= 0 && depth > opts->max_depth)
        return 0;

    char **local_ignore_patterns = NULL;
    int local_ignore_n = 0;
    if (!opts->no_ignore)
        bx_ignore_load_patterns(dirpath, opts, &local_ignore_patterns, &local_ignore_n);

    struct bx_ignore_state ignore_state;
    bx_ignore_state_init(&ignore_state, parent_ignore_state,
                         dirpath,
                         local_ignore_patterns, local_ignore_n);

    DIR *d = opendir(dirpath);
    if (!d) {
        if (errno == EACCES && opts->suppress_eacces) {
            if (opts->report_eacces)
                walk_report_error(opts, dirpath, errno);
            bx_ignore_state_dispose(&ignore_state);
            return 0;
        }
        walk_report_error(opts, dirpath, errno);
        bx_ignore_state_dispose(&ignore_state);
        return -1;
    }

    int status = 0;
    struct walk_dirent_list dirents = {0};
    if (!walk_dirent_list_read_sorted(d, &dirents)) {
        walk_report_error(opts, dirpath, ENOMEM);
        closedir(d);
        bx_ignore_state_dispose(&ignore_state);
        return -1;
    }

    for (size_t iter_index = 0; iter_index < dirents.len; iter_index++) {
        size_t dirent_index = opts->reverse_sort ? (dirents.len - 1 - iter_index) : iter_index;
        const struct walk_dirent_item *dirent_item = &dirents.items[dirent_index];
        if (walk_should_stop(opts))
            break;

        size_t plen = strlen(dirpath) + 1 + strlen(dirent_item->name) + 1;
        char *full = malloc(plen);
        snprintf(full, plen, "%s/%s", dirpath, dirent_item->name);

        /* Keep per-entry policy out of the walker; this loop should stay about
         * traversal, metadata, callbacks, and cycle/error handling. */
        if (bx_walk_filter_should_skip(filters, dirent_item->name, full, &ignore_state)) {
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
        bool cycle_check_ready = false;
        bool crosses_filesystem = false;
        bool repeated_dir = false;
        struct walk_entry entry = {
            .path = full,
            .follow_metadata = opts->follow_symlinks,
            .depth = depth + 1,
        };

        if (!opts->follow_symlinks) {
            if (dirent_item->d_type == DT_DIR) {
                entry.is_dir = true;
            } else if (dirent_item->d_type != DT_UNKNOWN) {
                entry.is_dir = false;
            } else {
                if (lstat(full, &st) != 0) {
                    free(full);
                    continue;
                }
                walk_entry_fill_from_stat(&entry, &st);
            }
        } else {
            if (dirent_item->d_type == DT_DIR) {
                entry.is_dir = true;
            } else if (dirent_item->d_type != DT_LNK && dirent_item->d_type != DT_UNKNOWN) {
                entry.is_dir = false;
            } else {
                if (lstat(full, &lst) != 0) {
                    free(full);
                    continue;
                }
                entry_was_symlink = S_ISLNK(lst.st_mode);
                if (!entry_was_symlink) {
                    if (stat(full, &st) != 0) {
                        free(full);
                        continue;
                    }
                    walk_entry_fill_from_stat(&entry, &st);
                } else if (stat(full, &st) == 0) {
                    walk_entry_fill_from_stat(&entry, &st);
                } else {
                    walk_entry_fill_from_stat(&entry, &lst);
                }
            }
        }

        if (entry_was_symlink && entry.is_dir && entry.metadata_loaded) {
            cycle_check_ready = true;
            crosses_filesystem = opts->stay_on_filesystem && entry.dev != opts->root_device;
            if (!crosses_filesystem) {
                if (opts->cycle_mode == WALK_CYCLE_DIR_REPEAT) {
                    repeated_dir = walk_ancestor_contains(ancestors, entry.dev, entry.inode);
                } else if (opts->cycle_mode == WALK_CYCLE_SYMLINK_REPEAT) {
                    repeated_dir = walk_ancestor_contains(ancestors, entry.dev, entry.inode);
                }
            }
        }

        if ((!opts->post_order || !entry.is_dir) && !repeated_dir)
            cb(&entry, user);

        if (!walk_should_stop(opts) && entry.is_dir && !entry.prune) {
            if (!cycle_check_ready) {
                if (!walk_entry_load_metadata(&entry)) {
                    free(full);
                    continue;
                }

                crosses_filesystem = opts->stay_on_filesystem && entry.dev != opts->root_device;
                if (!crosses_filesystem) {
                    if (opts->cycle_mode == WALK_CYCLE_DIR_REPEAT) {
                        repeated_dir = walk_ancestor_contains(ancestors, entry.dev, entry.inode);
                    } else if (opts->cycle_mode == WALK_CYCLE_SYMLINK_REPEAT) {
                        repeated_dir = entry_was_symlink &&
                                       walk_ancestor_contains(ancestors, entry.dev, entry.inode);
                    }
                }
            }

            if (repeated_dir) {
                walk_report_loop(opts, full);
                if (opts->cycle_report == WALK_CYCLE_ERROR)
                    status = -1;
            } else if (!crosses_filesystem) {
                struct walk_ancestor next = {
                    .dev = entry.dev,
                    .ino = entry.inode,
                    .path = full,
                    .parent = ancestors,
                };
                if (walk_recursive(full, opts, cb, user, depth + 1, &next,
                                   &ignore_state, filters) != 0)
                    status = -1;
            }
        }
        if (!walk_should_stop(opts) && opts->post_order && entry.is_dir && !repeated_dir)
            cb(&entry, user);
        free(full);
    }
    closedir(d);
    walk_dirent_list_free(&dirents);
    bx_ignore_state_dispose(&ignore_state);
    return status;
}

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user) {
    struct walk_opts effective_opts = *opts;
    struct stat st;
    int root_stat_rc = (opts && opts->follow_root_symlink) ? stat(root, &st) : lstat(root, &st);
    if (root_stat_rc != 0) {
        walk_report_error(opts, root, errno);
        return -1;
    }

    effective_opts.gitignore_enabled = bx_ignore_enable_gitignore_for_root(root, opts);
    if (effective_opts.stay_on_filesystem)
        effective_opts.root_device = st.st_dev;
    struct bx_walk_filter_state filters;
    bx_walk_filter_init(&filters, &effective_opts, root);

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
        if (walk_should_stop(opts) || entry.prune)
            return 0;
        struct walk_ancestor root_ancestor = {
            .dev = st.st_dev,
            .ino = st.st_ino,
            .path = root,
            .parent = NULL,
        };
        bool parent_ignore_ok = false;
        struct bx_ignore_state *parent_ignore_state =
            bx_ignore_load_parent_state(root, &effective_opts, &parent_ignore_ok);
        if (!parent_ignore_ok)
            return -1;
        int rc = walk_recursive(root, &effective_opts, cb, user, 0, &root_ancestor,
                                parent_ignore_state, &filters);
        bx_ignore_state_dispose_chain(parent_ignore_state);
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
