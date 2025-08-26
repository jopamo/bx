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

static bool load_ignore_patterns(const char *dirpath, char ***patterns, int *n) {
    size_t plen = strlen(dirpath) + 20;
    char *gipath = malloc(plen);
    snprintf(gipath, plen, "%s/.gitignore", dirpath);
    FILE *f = fopen(gipath, "r");
    free(gipath);
    if (!f) return false;

    int cap = 16;
    *patterns = malloc((size_t)cap * sizeof(char *));
    *n = 0;
    char *line = NULL;
    size_t lcap = 0;
    while (getline(&line, &lcap, f) != -1) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        if (*n >= cap) {
            cap *= 2;
            *patterns = realloc(*patterns, (size_t)cap * sizeof(char *));
        }
        (*patterns)[*n] = strdup(line);
        (*n)++;
    }
    free(line);
    fclose(f);
    return *n > 0;
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

static int walk_recursive(const char *dirpath, struct walk_opts *opts,
                          walk_callback cb, void *user, int depth,
                          const struct walk_ancestor *ancestors) {
    if (walk_should_stop(opts))
        return 0;

    if (opts->max_depth >= 0 && depth > opts->max_depth)
        return 0;

    char **ignore_patterns = NULL;
    int ignore_n = 0;
    if (!opts->no_ignore)
        load_ignore_patterns(dirpath, &ignore_patterns, &ignore_n);

    DIR *d = opendir(dirpath);
    if (!d) {
        if (errno == EACCES && opts->suppress_eacces) {
            for (int i = 0; i < ignore_n; i++) free(ignore_patterns[i]);
            free(ignore_patterns);
            return 0;
        }
        walk_report_error(opts, dirpath, errno);
        for (int i = 0; i < ignore_n; i++) free(ignore_patterns[i]);
        free(ignore_patterns);
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

        if (opts->max_depth >= 0 && depth + 1 > opts->max_depth) {
            free(full);
            continue;
        }

        struct stat st;
        struct stat lst;
        bool have_lstat = false;
        bool entry_was_symlink = false;

        if (opts->follow_symlinks) {
            if (lstat(full, &lst) != 0) {
                free(full);
                continue;
            }
            have_lstat = true;
            entry_was_symlink = S_ISLNK(lst.st_mode);
            if (stat(full, &st) != 0) {
                free(full);
                continue;
            }
        } else {
            if (lstat(full, &st) != 0) {
                free(full);
                continue;
            }
        }

        struct walk_entry entry = {
            .path = full,
            .is_dir = S_ISDIR(st.st_mode),
            .mode = st.st_mode,
            .inode = st.st_ino,
            .nlink = st.st_nlink,
            .uid = st.st_uid,
            .gid = st.st_gid,
            .size = st.st_size,
            .atime = st.st_atim,
            .mtime = st.st_mtim,
            .ctime = st.st_ctim,
            .depth = depth + 1,
        };
        if (!opts->post_order || !entry.is_dir)
            cb(&entry, user);

        if (!walk_should_stop(opts) && entry.is_dir) {
            bool repeated_dir = false;
            if (opts->cycle_mode == WALK_CYCLE_DIR_REPEAT) {
                repeated_dir = walk_ancestor_contains(ancestors, st.st_dev, st.st_ino);
            } else if (opts->cycle_mode == WALK_CYCLE_SYMLINK_REPEAT) {
                if (!have_lstat && opts->follow_symlinks) {
                    if (lstat(full, &lst) == 0) {
                        have_lstat = true;
                        entry_was_symlink = S_ISLNK(lst.st_mode);
                    }
                }
                repeated_dir = entry_was_symlink &&
                               walk_ancestor_contains(ancestors, st.st_dev, st.st_ino);
            }

            if (repeated_dir) {
                walk_report_loop(opts, full);
                if (opts->cycle_report == WALK_CYCLE_ERROR)
                    status = -1;
            } else {
                struct walk_ancestor next = {
                    .dev = st.st_dev,
                    .ino = st.st_ino,
                    .path = full,
                    .parent = ancestors,
                };
                if (walk_recursive(full, opts, cb, user, depth + 1, &next) != 0)
                    status = -1;
            }
        }
        if (!walk_should_stop(opts) && opts->post_order && entry.is_dir)
            cb(&entry, user);
        free(full);
    }
    closedir(d);
    for (int i = 0; i < ignore_n; i++) free(ignore_patterns[i]);
    free(ignore_patterns);
    return status;
}

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user) {
    struct stat st;
    int root_stat_rc = (opts && opts->follow_root_symlink) ? stat(root, &st) : lstat(root, &st);
    if (root_stat_rc != 0) {
        walk_report_error(opts, root, errno);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        struct walk_entry entry = {
            .path = strdup(root),
            .is_dir = true,
            .mode = st.st_mode,
            .inode = st.st_ino,
            .nlink = st.st_nlink,
            .uid = st.st_uid,
            .gid = st.st_gid,
            .size = st.st_size,
            .atime = st.st_atim,
            .mtime = st.st_mtim,
            .ctime = st.st_ctim,
            .depth = 0,
        };
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
        int rc = walk_recursive(root, opts, cb, user, 0, &root_ancestor);
        if (!walk_should_stop(opts) && opts->post_order) {
            struct walk_entry post = {
                .path = strdup(root),
                .is_dir = true,
                .mode = st.st_mode,
                .inode = st.st_ino,
                .nlink = st.st_nlink,
                .uid = st.st_uid,
                .gid = st.st_gid,
                .size = st.st_size,
                .atime = st.st_atim,
                .mtime = st.st_mtim,
                .ctime = st.st_ctim,
                .depth = 0,
            };
            cb(&post, user);
            free(post.path);
        }
        return rc;
    }

    struct walk_entry entry = {
        .path = strdup(root),
        .is_dir = false,
        .mode = st.st_mode,
        .inode = st.st_ino,
        .nlink = st.st_nlink,
        .uid = st.st_uid,
        .gid = st.st_gid,
        .size = st.st_size,
        .atime = st.st_atim,
        .mtime = st.st_mtim,
        .ctime = st.st_ctim,
        .depth = 0,
    };
    cb(&entry, user);
    free(entry.path);
    return 0;
}
