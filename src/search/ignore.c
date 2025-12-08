#define _GNU_SOURCE
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"

enum bx_ignore_match_result {
    BX_IGNORE_NO_MATCH = 0,
    BX_IGNORE_INCLUDE,
    BX_IGNORE_EXCLUDE,
};

static enum bx_ignore_match_result match_ignore_line(const char *line,
                                                     const char *name,
                                                     const char *relative_path) {
    const char *p = line;
    while (*p == ' ')
        p++;
    if (*p == '#' || *p == '\0')
        return BX_IGNORE_NO_MATCH;

    bool negate = false;
    if (*p == '!') {
        negate = true;
        p++;
    }
    if (*p == '/')
        p++;
    if (*p == '\0')
        return BX_IGNORE_NO_MATCH;

    bool match = false;
    if (strchr(p, '/')) {
        match = relative_path && relative_path[0] != '\0' &&
                fnmatch(p, relative_path, FNM_PATHNAME) == 0;
    } else {
        match = fnmatch(p, name, FNM_PATHNAME) == 0;
    }

    if (!match)
        return BX_IGNORE_NO_MATCH;
    return negate ? BX_IGNORE_INCLUDE : BX_IGNORE_EXCLUDE;
}

static const char *bx_ignore_state_relative_path(const struct bx_ignore_state *state,
                                                 const char *path,
                                                 const char *root_relative_path) {
    if (!state || !state->dirpath || !path)
        goto parent_prefix;

    size_t dir_len = strlen(state->dirpath);
    if (strncmp(path, state->dirpath, dir_len) != 0)
        goto parent_prefix;
    if (path[dir_len] == '/')
        return path + dir_len + 1;
    if (path[dir_len] == '\0')
        return path + dir_len;

parent_prefix:
    if (!state || !state->root_prefix)
        return NULL;
    return root_relative_path;
}

static enum bx_ignore_match_result bx_ignore_match_patterns(const char *name,
                                                            const char *relative_path,
                                                            char **patterns,
                                                            int n) {
    if (!patterns || n <= 0)
        return BX_IGNORE_NO_MATCH;

    for (int i = n - 1; i >= 0; i--) {
        enum bx_ignore_match_result result = match_ignore_line(patterns[i], name, relative_path);
        if (result != BX_IGNORE_NO_MATCH)
            return result;
    }

    return BX_IGNORE_NO_MATCH;
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
                          struct bx_ignore_state *parent,
                          const char *dirpath,
                          char **patterns, int pattern_count) {
    if (!state)
        return;
    state->parent = parent;
    state->dirpath = dirpath;
    state->owned_dirpath = NULL;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->patterns = patterns;
    state->pattern_count = pattern_count;
}

void bx_ignore_state_dispose(struct bx_ignore_state *state) {
    if (!state)
        return;
    bx_ignore_free_patterns(state->patterns, state->pattern_count);
    state->parent = NULL;
    free(state->owned_dirpath);
    free(state->owned_root_prefix);
    state->dirpath = NULL;
    state->owned_dirpath = NULL;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->patterns = NULL;
    state->pattern_count = 0;
}

void bx_ignore_state_dispose_chain(struct bx_ignore_state *state) {
    while (state) {
        struct bx_ignore_state *parent = state->parent;
        bx_ignore_state_dispose(state);
        free(state);
        state = parent;
    }
}

bool bx_ignore_load_patterns(const char *dirpath, const struct bx_walk_ignore_opts *opts,
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
    char *git_path = bx_path_join(dirpath, ".git");
    if (!git_path)
        return false;
    struct stat st;
    bool found = lstat(git_path, &st) == 0;
    free(git_path);
    return found;
}

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct bx_walk_ignore_opts *opts) {
    if (!opts || opts->no_ignore || opts->no_ignore_vcs)
        return false;
    if (opts->no_require_git)
        return true;

    char *resolved = bx_path_realpath_dup(root);
    if (!resolved)
        return false;

    bool found = false;
    char *cursor = resolved;
    bool cursor_owned = false;
    while (cursor && cursor[0] != '\0') {
        if (path_has_git_dir(cursor)) {
            found = true;
            break;
        }
        if (strcmp(cursor, "/") == 0)
            break;
        char *parent = bx_path_parent_dir_stripped_dup(cursor);
        if (cursor_owned)
            free(cursor);
        cursor = parent;
        cursor_owned = true;
    }

    if (cursor_owned)
        free(cursor);
    free(resolved);
    return found;
}

struct bx_ignore_state *bx_ignore_load_parent_state(const char *root,
                                                    const struct bx_walk_ignore_opts *opts,
                                                    bool *ok) {
    if (ok)
        *ok = false;

    if (!root || !opts || opts->no_ignore || opts->no_ignore_parent)
        goto success;

    char *resolved_root = bx_path_realpath_dup(root);
    if (!resolved_root)
        goto success;

    char *cursor = bx_path_parent_dir_stripped_dup(resolved_root);
    if (!cursor) {
        free(resolved_root);
        goto fail;
    }

    char **dirs = NULL;
    int dir_count = 0;
    int dir_cap = 0;
    struct bx_ignore_state *chain = NULL;

    while (cursor && strcmp(cursor, "/") != 0) {
        if (dir_count >= dir_cap) {
            int new_cap = dir_cap == 0 ? 8 : dir_cap * 2;
            char **tmp = realloc(dirs, (size_t)new_cap * sizeof(*dirs));
            if (!tmp) {
                free(cursor);
                free(dirs);
                free(resolved_root);
                goto fail;
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
            free(resolved_root);
            goto fail;
        }
        char *parent = bx_path_parent_dir_stripped_dup(cursor);
        free(cursor);
        cursor = parent;
    }
    free(cursor);

    for (int i = dir_count - 1; i >= 0; i--) {
        char **loaded = NULL;
        int loaded_n = 0;
        bx_ignore_load_patterns(dirs[i], opts, &loaded, &loaded_n);
        if (loaded_n > 0) {
            struct bx_ignore_state *state = calloc(1, sizeof(*state));
            if (!state) {
                bx_ignore_free_patterns(loaded, loaded_n);
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                bx_ignore_state_dispose_chain(chain);
                free(resolved_root);
                goto fail;
            }
            bx_ignore_state_init(state, chain, NULL, loaded, loaded_n);
            size_t dir_len = strlen(dirs[i]);
            const char *relative_root = resolved_root;
            if (strncmp(resolved_root, dirs[i], dir_len) == 0) {
                if (resolved_root[dir_len] == '/')
                    relative_root = resolved_root + dir_len + 1;
                else if (resolved_root[dir_len] == '\0')
                    relative_root = "";
            }
            state->owned_root_prefix = strdup(relative_root);
            if (!state->owned_root_prefix) {
                free(state);
                bx_ignore_free_patterns(loaded, loaded_n);
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                bx_ignore_state_dispose_chain(chain);
                free(resolved_root);
                goto fail;
            }
            state->root_prefix = state->owned_root_prefix;
            chain = state;
        } else {
            bx_ignore_free_patterns(loaded, loaded_n);
        }
    }

    for (int i = 0; i < dir_count; i++)
        free(dirs[i]);
    free(dirs);
    free(resolved_root);
    if (ok)
        *ok = true;
    return chain;

fail:
    if (ok)
        *ok = false;
    return NULL;

success:
    if (ok)
        *ok = true;
    return NULL;
}

bool bx_ignore_path_ignored(const char *name, char **patterns, int n) {
    return bx_ignore_match_patterns(name, NULL, patterns, n) == BX_IGNORE_EXCLUDE;
}

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state,
                                  const char *name,
                                  const char *path,
                                  const char *root_relative_path) {
    for (const struct bx_ignore_state *it = state; it; it = it->parent) {
        const char *relative_path = bx_ignore_state_relative_path(it, path, root_relative_path);
        char *prefixed_path = NULL;
        if (it->root_prefix && it->root_prefix[0] != '\0') {
            size_t prefix_len = strlen(it->root_prefix);
            size_t rel_len = relative_path ? strlen(relative_path) : 0;
            size_t total = prefix_len + (rel_len > 0 ? 1 + rel_len : 0) + 1;
            prefixed_path = malloc(total);
            if (!prefixed_path)
                return false;
            memcpy(prefixed_path, it->root_prefix, prefix_len);
            if (rel_len > 0) {
                prefixed_path[prefix_len] = '/';
                memcpy(prefixed_path + prefix_len + 1, relative_path, rel_len);
                prefixed_path[prefix_len + 1 + rel_len] = '\0';
            } else {
                prefixed_path[prefix_len] = '\0';
            }
            relative_path = prefixed_path;
        }
        enum bx_ignore_match_result result =
            bx_ignore_match_patterns(name, relative_path, it->patterns, it->pattern_count);
        free(prefixed_path);
        if (result == BX_IGNORE_EXCLUDE)
            return true;
        if (result == BX_IGNORE_INCLUDE)
            return false;
    }
    return false;
}
