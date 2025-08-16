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

static bool walk_should_stop(const struct walk_opts *opts) {
    return opts->stop && *opts->stop;
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
                          walk_callback cb, void *user, int depth) {
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
        if (errno != EACCES)
            fprintf(stderr, "walk: %s: %s\n", dirpath, strerror(errno));
        for (int i = 0; i < ignore_n; i++) free(ignore_patterns[i]);
        free(ignore_patterns);
        return -1;
    }

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

        struct stat st;
        int stat_rc = opts->follow_symlinks ? stat(full, &st) : lstat(full, &st);
        if (stat_rc != 0) { free(full); continue; }

        struct walk_entry entry = {.path = full, .is_dir = S_ISDIR(st.st_mode)};
        cb(&entry, user);

        if (!walk_should_stop(opts) && entry.is_dir) {
            walk_recursive(full, opts, cb, user, depth + 1);
        }
        free(full);
    }
    closedir(d);
    for (int i = 0; i < ignore_n; i++) free(ignore_patterns[i]);
    free(ignore_patterns);
    return 0;
}

int walk_dir(const char *root, struct walk_opts *opts, walk_callback cb, void *user) {
    struct stat st;
    if (stat(root, &st) != 0) {
        fprintf(stderr, "walk: %s: %s\n", root, strerror(errno));
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        struct walk_entry entry = {.path = strdup(root), .is_dir = true};
        cb(&entry, user);
        free(entry.path);
        if (walk_should_stop(opts))
            return 0;
        return walk_recursive(root, opts, cb, user, 0);
    }

    struct walk_entry entry = {.path = strdup(root), .is_dir = false};
    cb(&entry, user);
    free(entry.path);
    return 0;
}
