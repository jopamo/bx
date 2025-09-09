#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static bool match_ignore_line(const char *line, const char *name) {
    if (line[0] == '#' || line[0] == '\0')
        return false;
    const char *p = line;
    while (*p == ' ') p++;
    bool negate = false;
    if (*p == '!') { negate = true; p++; }
    bool match = fnmatch(p, name, FNM_PATHNAME) == 0;
    return negate ? !match : match;
}

static bool append_ignore_pattern(char ***patterns, int *n, int *cap, const char *pattern) {
    if (*n >= *cap) {
        int new_cap = *cap == 0 ? 16 : *cap * 2;
        char **tmp = realloc(*patterns, (size_t)new_cap * sizeof(**patterns));
        if (!tmp)
            return false;
        *patterns = tmp;
        *cap = new_cap;
    }

    (*patterns)[*n] = strdup(pattern);
    if (!(*patterns)[*n])
        return false;
    (*n)++;
    return true;
}

static void free_ignore_patterns(char **patterns, int n) {
    if (!patterns)
        return;
    for (int i = 0; i < n; i++)
        free(patterns[i]);
    free(patterns);
}

static bool clone_ignore_patterns(char **src, int src_n, char ***dst, int *dst_n, int *dst_cap) {
    *dst = NULL;
    *dst_n = 0;
    *dst_cap = 0;

    for (int i = 0; i < src_n; i++) {
        if (!append_ignore_pattern(dst, dst_n, dst_cap, src[i])) {
            free_ignore_patterns(*dst, *dst_n);
            *dst = NULL;
            *dst_n = 0;
            *dst_cap = 0;
            return false;
        }
    }
    return true;
}

static bool load_ignore_patterns(const char *dirpath, const struct walk_opts *opts,
                                 char ***patterns, int *n) {
    *patterns = NULL;
    *n = 0;

    if (!opts || !opts->ignore_filenames || opts->num_ignore_filenames <= 0)
        return false;

    int cap = 0;
    bool loaded_any = false;

    for (int file_i = 0; file_i < opts->num_ignore_filenames; file_i++) {
        const char *filename = opts->ignore_filenames[file_i];
        if (!filename || filename[0] == '\0')
            continue;
        if (opts->no_ignore_vcs && strcmp(filename, ".gitignore") == 0)
            continue;
        if (!opts->no_require_git && strcmp(filename, ".gitignore") == 0) {
            continue;
        }
        if (opts->no_ignore_dot &&
            (strcmp(filename, ".ignore") == 0 || strcmp(filename, ".rgignore") == 0))
            continue;

        size_t plen = strlen(dirpath) + 1 + strlen(filename) + 1;
        char *ignore_path = malloc(plen);
        if (!ignore_path)
            continue;

        snprintf(ignore_path, plen, "%s/%s", dirpath, filename);
        FILE *f = fopen(ignore_path, "r");
        free(ignore_path);
        if (!f)
            continue;

        loaded_any = true;
        char *line = NULL;
        size_t lcap = 0;
        while (getline(&line, &lcap, f) != -1) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (len == 0 || line[0] == '#')
                continue;
            if (!append_ignore_pattern(patterns, n, &cap, line)) {
                free(line);
                fclose(f);
                return loaded_any || *n > 0;
            }
        }
        free(line);
        fclose(f);
    }

    return loaded_any && *n > 0;
}

static bool path_has_git_dir(const char *dirpath) {
    size_t plen = strlen(dirpath) + strlen("/.git") + 1;
    char *git_path = malloc(plen);
    if (!git_path)
        return false;
    snprintf(git_path, plen, "%s/.git", dirpath);
    struct stat st;
    bool found = lstat(git_path, &st) == 0;
    free(git_path);
    return found;
}

static bool enable_gitignore_for_root(const char *root, const struct walk_opts *opts) {
    if (!opts || opts->no_ignore || opts->no_ignore_vcs)
        return false;
    if (opts->no_require_git)
        return true;

    char *resolved = realpath(root, NULL);
    if (!resolved)
        return false;

    bool found = false;
    char *cursor = resolved;
    while (cursor && cursor[0] != '\0') {
        if (path_has_git_dir(cursor)) {
            found = true;
            break;
        }
        char *slash = strrchr(cursor, '/');
        if (!slash)
            break;
        if (slash == cursor) {
            if (path_has_git_dir("/"))
                found = true;
            break;
        }
        *slash = '\0';
    }

    free(resolved);
    return found;
}

static bool load_parent_ignore_patterns(const char *root, const struct walk_opts *opts,
                                        char ***patterns, int *n) {
    *patterns = NULL;
    *n = 0;

    if (!root || !opts || opts->no_ignore || opts->no_ignore_parent)
        return true;

    char *resolved_root = realpath(root, NULL);
    if (!resolved_root)
        return true;

    char *cursor = strdup(resolved_root);
    free(resolved_root);
    if (!cursor)
        return false;

    char **dirs = NULL;
    int dir_count = 0;
    int dir_cap = 0;

    while (cursor && strcmp(cursor, "/") != 0) {
        char *slash = strrchr(cursor, '/');
        if (!slash)
            break;
        if (slash == cursor) {
            break;
        }
        *slash = '\0';
        if (dir_count >= dir_cap) {
            int new_cap = dir_cap == 0 ? 8 : dir_cap * 2;
            char **tmp = realloc(dirs, (size_t)new_cap * sizeof(*dirs));
            if (!tmp) {
                free(cursor);
                free(dirs);
                return false;
            }
            dirs = tmp;
            dir_cap = new_cap;
        }
        dirs[dir_count++] = strdup(cursor);
        if (!dirs[dir_count - 1]) {
            free(cursor);
            for (int i = 0; i < dir_count - 1; i++)
                free(dirs[i]);
            free(dirs);
            return false;
        }
    }
    free(cursor);

    int cap = 0;
    for (int i = dir_count - 1; i >= 0; i--) {
        char **loaded = NULL;
        int loaded_n = 0;
        load_ignore_patterns(dirs[i], opts, &loaded, &loaded_n);
        for (int j = 0; j < loaded_n; j++) {
            if (!append_ignore_pattern(patterns, n, &cap, loaded[j])) {
                free_ignore_patterns(loaded, loaded_n);
                free_ignore_patterns(*patterns, *n);
                *patterns = NULL;
                *n = 0;
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                return false;
            }
        }
        free_ignore_patterns(loaded, loaded_n);
    }

    for (int i = 0; i < dir_count; i++)
        free(dirs[i]);
    free(dirs);
    return true;
}

static bool is_ignored(const char *name, char **patterns, int n) {
    if (patterns && n > 0) {
        for (int i = n - 1; i >= 0; i--) {
            if (match_ignore_line(patterns[i], name))
                return true;
        }
    }
    return false;
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
        load_ignore_patterns(dirpath, opts, &local_ignore_patterns, &local_ignore_n);

    char **ignore_patterns = NULL;
    int ignore_n = 0;
    int ignore_cap = 0;
    if (!clone_ignore_patterns(parent_ignore_patterns, parent_ignore_n,
                               &ignore_patterns, &ignore_n, &ignore_cap)) {
        free_ignore_patterns(local_ignore_patterns, local_ignore_n);
        return -1;
    }
    for (int i = 0; i < local_ignore_n; i++) {
        if (!append_ignore_pattern(&ignore_patterns, &ignore_n, &ignore_cap,
                                   local_ignore_patterns[i])) {
            free_ignore_patterns(local_ignore_patterns, local_ignore_n);
            free_ignore_patterns(ignore_patterns, ignore_n);
            return -1;
        }
    }
    free_ignore_patterns(local_ignore_patterns, local_ignore_n);

    DIR *d = opendir(dirpath);
    if (!d) {
        if (errno == EACCES && opts->suppress_eacces) {
            if (opts->report_eacces)
                walk_report_error(opts, dirpath, errno);
            free_ignore_patterns(ignore_patterns, ignore_n);
            return 0;
        }
        walk_report_error(opts, dirpath, errno);
        free_ignore_patterns(ignore_patterns, ignore_n);
        return -1;
    }

    int status = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (walk_should_stop(opts))
            break;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (!opts->hidden && is_hidden(ent->d_name))
            continue;

        if (is_ignored(ent->d_name, ignore_patterns, ignore_n))
            continue;

        if (opts->exclude_dirs) {
            bool skip = false;
            for (int e = 0; e < opts->num_exclude_dirs; e++)
                if (fnmatch(opts->exclude_dirs[e], ent->d_name, 0) == 0) skip = true;
            if (skip) continue;
        }

        size_t plen = strlen(dirpath) + 1 + strlen(ent->d_name) + 1;
        char *full = malloc(plen);
        snprintf(full, plen, "%s/%s", dirpath, ent->d_name);

        const char *relative_path = walk_relative_path(root_path, full);

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
    free_ignore_patterns(ignore_patterns, ignore_n);
    return status;
}

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user) {
    struct walk_opts effective_opts = *opts;
    if (!enable_gitignore_for_root(root, opts))
        effective_opts.no_require_git = false;
    else
        effective_opts.no_require_git = true;

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
        if (!load_parent_ignore_patterns(root, &effective_opts, &parent_ignore_patterns, &parent_ignore_n))
            return -1;
        int rc = walk_recursive(root, &effective_opts, cb, user, 0, &root_ancestor,
                                parent_ignore_patterns, parent_ignore_n, root);
        free_ignore_patterns(parent_ignore_patterns, parent_ignore_n);
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
