#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ignore.h"
#include "walk.h"

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

bool bx_ignore_append_pattern(char ***patterns, int *n, int *cap, const char *pattern) {
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

void bx_ignore_free_patterns(char **patterns, int n) {
    if (!patterns)
        return;
    for (int i = 0; i < n; i++)
        free(patterns[i]);
    free(patterns);
}

void bx_ignore_state_init(struct bx_ignore_state *state,
                          const struct bx_ignore_state *parent,
                          char **patterns, int pattern_count) {
    if (!state)
        return;
    state->parent = parent;
    state->patterns = patterns;
    state->pattern_count = pattern_count;
}

void bx_ignore_state_dispose(struct bx_ignore_state *state) {
    if (!state)
        return;
    bx_ignore_free_patterns(state->patterns, state->pattern_count);
    state->parent = NULL;
    state->patterns = NULL;
    state->pattern_count = 0;
}

bool bx_ignore_load_patterns(const char *dirpath, const struct walk_opts *opts,
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
        if (!opts->gitignore_enabled && strcmp(filename, ".gitignore") == 0)
            continue;
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
            if (!bx_ignore_append_pattern(patterns, n, &cap, line)) {
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

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct walk_opts *opts) {
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

bool bx_ignore_load_parent_patterns(const char *root, const struct walk_opts *opts,
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
        if (slash == cursor)
            break;
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
        bx_ignore_load_patterns(dirs[i], opts, &loaded, &loaded_n);
        for (int j = 0; j < loaded_n; j++) {
            if (!bx_ignore_append_pattern(patterns, n, &cap, loaded[j])) {
                bx_ignore_free_patterns(loaded, loaded_n);
                bx_ignore_free_patterns(*patterns, *n);
                *patterns = NULL;
                *n = 0;
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                return false;
            }
        }
        bx_ignore_free_patterns(loaded, loaded_n);
    }

    for (int i = 0; i < dir_count; i++)
        free(dirs[i]);
    free(dirs);
    return true;
}

bool bx_ignore_path_ignored(const char *name, char **patterns, int n) {
    if (patterns && n > 0) {
        for (int i = n - 1; i >= 0; i--) {
            if (match_ignore_line(patterns[i], name))
                return true;
        }
    }
    return false;
}

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state, const char *name) {
    for (const struct bx_ignore_state *it = state; it; it = it->parent) {
        if (bx_ignore_path_ignored(name, it->patterns, it->pattern_count))
            return true;
    }
    return false;
}
