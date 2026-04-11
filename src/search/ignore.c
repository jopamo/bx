#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"

#define BX_IGNORE_STACK_PATH_BUFSIZE 512u

enum bx_ignore_match_result {
    BX_IGNORE_NO_MATCH = 0,
    BX_IGNORE_INCLUDE,
    BX_IGNORE_EXCLUDE,
};

static enum bx_ignore_match_result match_ignore_line(const char *line,
                                                     const char *name,
                                                     const char *relative_path,
                                                     bool casefold) {
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
    int flags = FNM_PATHNAME | (casefold ? FNM_CASEFOLD : 0);
    if (strchr(p, '/')) {
        match = relative_path && relative_path[0] != '\0' &&
                fnmatch(p, relative_path, flags) == 0;
    } else {
        match = fnmatch(p, name, flags) == 0;
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

    size_t dir_len = state->dirpath_len;
    if (strncmp(path, state->dirpath, dir_len) != 0)
        goto parent_prefix;
    if (path[dir_len] == '/')
        return path + dir_len + 1;
    if (path[dir_len] == '\0')
        return path + dir_len;

parent_prefix:
    if (!state || !state->root_prefix)
        return path;
    return root_relative_path;
}

static enum bx_ignore_match_result bx_ignore_match_patterns(const char *name,
                                                            const char *relative_path,
                                                            char **patterns,
                                                            int n,
                                                            bool casefold) {
    if (!patterns || n <= 0)
        return BX_IGNORE_NO_MATCH;

    for (int i = n - 1; i >= 0; i--) {
        enum bx_ignore_match_result result =
            match_ignore_line(patterns[i], name, relative_path, casefold);
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
    state->dirpath_len = dirpath ? strlen(dirpath) : 0u;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->root_prefix_len = 0u;
    state->patterns = patterns;
    state->pattern_count = pattern_count;
    state->casefold = false;
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
    state->dirpath_len = 0u;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->root_prefix_len = 0u;
    state->patterns = NULL;
    state->pattern_count = 0;
    state->casefold = false;
}

void bx_ignore_state_dispose_chain(struct bx_ignore_state *state) {
    while (state) {
        struct bx_ignore_state *parent = state->parent;
        bx_ignore_state_dispose(state);
        free(state);
        state = parent;
    }
}

static char **bx_ignore_clone_patterns(char **patterns, int pattern_count) {
    if (!patterns || pattern_count <= 0)
        return NULL;

    char **copy = calloc((size_t)pattern_count, sizeof(*copy));
    if (!copy)
        return NULL;

    for (int i = 0; i < pattern_count; ++i) {
        copy[i] = patterns[i] ? strdup(patterns[i]) : NULL;
        if (patterns[i] && !copy[i]) {
            bx_ignore_free_patterns(copy, i);
            return NULL;
        }
    }
    return copy;
}

struct bx_ignore_state *bx_ignore_state_clone_chain(const struct bx_ignore_state *state) {
    if (!state)
        return NULL;

    struct bx_ignore_state *parent = bx_ignore_state_clone_chain(state->parent);
    char **patterns = bx_ignore_clone_patterns(state->patterns, state->pattern_count);
    struct bx_ignore_state *copy = calloc(1u, sizeof(*copy));
    if (!copy) {
        bx_ignore_free_patterns(patterns, state->pattern_count);
        bx_ignore_state_dispose_chain(parent);
        return NULL;
    }

    bx_ignore_state_init(copy, parent, state->dirpath, patterns, state->pattern_count);
    if (state->dirpath) {
        copy->owned_dirpath = strdup(state->dirpath);
        if (!copy->owned_dirpath) {
            bx_ignore_state_dispose(copy);
            free(copy);
            bx_ignore_state_dispose_chain(parent);
            return NULL;
        }
        copy->dirpath = copy->owned_dirpath;
        copy->dirpath_len = strlen(copy->owned_dirpath);
    }
    if (state->root_prefix) {
        copy->owned_root_prefix = strdup(state->root_prefix);
        if (!copy->owned_root_prefix) {
            bx_ignore_state_dispose(copy);
            free(copy);
            bx_ignore_state_dispose_chain(parent);
            return NULL;
        }
        copy->root_prefix = copy->owned_root_prefix;
        copy->root_prefix_len = strlen(copy->owned_root_prefix);
    }
    copy->casefold = state->casefold;
    return copy;
}

static bool bx_ignore_state_rewrite_root_prefixes(struct bx_ignore_state *state,
                                                  const char *current_root,
                                                  const char *subtree_root) {
    if (!state)
        return true;
    if (!bx_ignore_state_rewrite_root_prefixes(state->parent, current_root, subtree_root))
        return false;

    char *old_root_prefix = state->root_prefix ? strdup(state->root_prefix) : NULL;
    if (state->root_prefix && !old_root_prefix)
        return false;

    free(state->owned_root_prefix);
    state->owned_root_prefix = NULL;
    state->root_prefix = NULL;
    state->root_prefix_len = 0u;

    if (state->dirpath) {
        free(old_root_prefix);
        return true;
    }

    const char *relative_root = NULL;
    if (!relative_root && current_root && subtree_root &&
        strncmp(subtree_root, current_root, strlen(current_root)) == 0) {
        const char *suffix = subtree_root + strlen(current_root);
        if (*suffix == '/')
            suffix++;
        else if (*suffix != '\0')
            suffix = NULL;
        if (suffix) {
            const char *base = old_root_prefix ? old_root_prefix : "";
            size_t base_len = strlen(base);
            size_t suffix_len = strlen(suffix);
            size_t total = base_len + (base_len > 0u && suffix_len > 0u ? 1u : 0u) + suffix_len + 1u;
            state->owned_root_prefix = malloc(total);
            if (!state->owned_root_prefix) {
                free(old_root_prefix);
                return false;
            }
            if (base_len > 0u)
                memcpy(state->owned_root_prefix, base, base_len);
            if (base_len > 0u && suffix_len > 0u)
                state->owned_root_prefix[base_len++] = '/';
            if (suffix_len > 0u)
                memcpy(state->owned_root_prefix + base_len, suffix, suffix_len);
            state->owned_root_prefix[base_len + suffix_len] = '\0';
            state->root_prefix = state->owned_root_prefix;
            state->root_prefix_len = base_len + suffix_len;
            free(old_root_prefix);
            return true;
        }
    }
    if (!relative_root) {
        free(old_root_prefix);
        return true;
    }

    state->owned_root_prefix = strdup(relative_root);
    if (!state->owned_root_prefix) {
        free(old_root_prefix);
        return false;
    }
    state->root_prefix = state->owned_root_prefix;
    state->root_prefix_len = strlen(state->owned_root_prefix);
    free(old_root_prefix);
    return true;
}

struct bx_ignore_state *bx_ignore_state_clone_chain_for_subtree(const struct bx_ignore_state *state,
                                                                const char *current_root,
                                                                const char *subtree_root) {
    struct bx_ignore_state *copy = bx_ignore_state_clone_chain(state);
    if (!copy)
        return NULL;
    if (bx_ignore_state_rewrite_root_prefixes(copy, current_root, subtree_root))
        return copy;
    bx_ignore_state_dispose_chain(copy);
    return NULL;
}

static void bx_ignore_report_error(const struct bx_walk_ignore_opts *opts,
                                   const char *path,
                                   int errnum) {
    if (!opts || opts->suppress_ignore_messages || !opts->error_prefix || !path)
        return;
    if (opts->os_error_style) {
        fprintf(stderr, "%s: %s: %s (os error %d)\n",
                opts->error_prefix, path, strerror(errnum), errnum);
    } else {
        fprintf(stderr, "%s: %s: %s\n", opts->error_prefix, path, strerror(errnum));
    }
}

static bool bx_ignore_load_patterns_from_path(const char *path,
                                              const struct bx_walk_ignore_opts *opts,
                                              char ***patterns, int *n,
                                              bool explicit_file) {
    FILE *f;
    int cap = *n;
    bool loaded_any = false;

    if (!patterns || !n)
        return false;

    f = fopen(path, "r");
    if (!f) {
        if (explicit_file)
            bx_ignore_report_error(opts, path, errno);
        return false;
    }

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
    return loaded_any && *n > 0;
}

static struct bx_ignore_state *bx_ignore_append_state(struct bx_ignore_state *chain,
                                                      char **patterns, int pattern_count,
                                                      const char *dirpath,
                                                      const char *root_prefix,
                                                      bool casefold) {
    struct bx_ignore_state *state;

    if (pattern_count <= 0) {
        bx_ignore_free_patterns(patterns, pattern_count);
        return chain;
    }

    state = calloc(1, sizeof(*state));
    if (!state) {
        bx_ignore_free_patterns(patterns, pattern_count);
        return NULL;
    }
    bx_ignore_state_init(state, chain, dirpath, patterns, pattern_count);
    if (dirpath) {
        state->owned_dirpath = strdup(dirpath);
        if (!state->owned_dirpath) {
            bx_ignore_state_dispose(state);
            free(state);
            return NULL;
        }
        state->dirpath = state->owned_dirpath;
        state->dirpath_len = strlen(state->owned_dirpath);
    }
    if (root_prefix) {
        state->owned_root_prefix = strdup(root_prefix);
        if (!state->owned_root_prefix) {
            bx_ignore_state_dispose(state);
            free(state);
            return NULL;
        }
        state->root_prefix = state->owned_root_prefix;
        state->root_prefix_len = strlen(state->owned_root_prefix);
    }
    state->casefold = casefold;
    return state;
}

bool bx_ignore_load_patterns(const char *dirpath, const struct bx_walk_ignore_opts *opts,
                             char ***patterns, int *n) {
    *patterns = NULL;
    *n = 0;

    if (!opts || !opts->ignore_filenames || opts->num_ignore_filenames <= 0)
        return false;

    bool loaded_any = false;

    for (int file_i = 0; file_i < opts->num_ignore_filenames; file_i++) {
        const char *filename = opts->ignore_filenames[file_i];
        struct stat st;
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
        if (stat(ignore_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(ignore_path);
            continue;
        }
        loaded_any |= bx_ignore_load_patterns_from_path(ignore_path, opts, patterns, n, false);
        free(ignore_path);
    }

    return loaded_any && *n > 0;
}

void bx_ignore_validate_explicit_ignore_files(const struct bx_walk_ignore_opts *opts) {
    if (!opts || opts->no_ignore_files || !opts->extra_ignore_files || opts->num_extra_ignore_files <= 0)
        return;

    for (int i = 0; i < opts->num_extra_ignore_files; i++) {
        const char *path = opts->extra_ignore_files[i];
        FILE *f;

        if (!path || path[0] == '\0')
            continue;
        f = fopen(path, "r");
        if (!f) {
            bx_ignore_report_error(opts, path, errno);
            continue;
        }
        fclose(f);
    }
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

char *bx_ignore_find_git_root(const char *root, const struct bx_walk_ignore_opts *opts) {
    char *resolved;
    char *cursor;
    char *found = NULL;

    if (!root || !opts || opts->no_ignore || opts->no_ignore_vcs)
        return NULL;
    if (opts->no_require_git)
        return bx_path_realpath_dup(root);

    resolved = bx_path_realpath_dup(root);
    if (!resolved)
        return NULL;

    cursor = resolved;
    while (cursor && cursor[0] != '\0') {
        if (path_has_git_dir(cursor)) {
            found = strdup(cursor);
            break;
        }
        if (strcmp(cursor, "/") == 0)
            break;
        char *parent = bx_path_parent_dir_stripped_dup(cursor);
        if (cursor != resolved)
            free(cursor);
        cursor = parent;
    }

    if (cursor && cursor != resolved)
        free(cursor);
    free(resolved);
    return found;
}

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct bx_walk_ignore_opts *opts) {
    char *git_root = bx_ignore_find_git_root(root, opts);
    if (git_root) {
        free(git_root);
        return true;
    }
    return opts && !opts->no_ignore && !opts->no_ignore_vcs && opts->no_require_git;
}

struct bx_ignore_state *bx_ignore_load_parent_state(const char *root,
                                                    const struct bx_walk_ignore_opts *opts,
                                                    bool *ok) {
    struct bx_ignore_state *chain = NULL;
    char *resolved_root = NULL;

    if (ok)
        *ok = false;

    if (!opts)
        goto success;

    if (!opts->no_ignore_files && opts->extra_ignore_files && opts->num_extra_ignore_files > 0) {
        for (int i = 0; i < opts->num_extra_ignore_files; i++) {
            char **loaded = NULL;
            int loaded_n = 0;
            bx_ignore_load_patterns_from_path(opts->extra_ignore_files[i], opts,
                                              &loaded, &loaded_n, false);
            if (loaded_n > 0) {
                chain = bx_ignore_append_state(chain, loaded, loaded_n, NULL, NULL,
                                               opts->ignore_file_case_insensitive);
                if (!chain)
                    goto fail;
            } else {
                bx_ignore_free_patterns(loaded, loaded_n);
            }
        }
    }

    if (!opts->no_ignore && !opts->no_ignore_global && opts->gitignore_enabled) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char *base = NULL;
        char *git_dir = NULL;
        char *global_path = NULL;
        char **loaded = NULL;
        int loaded_n = 0;

        if (xdg && *xdg) {
            base = strdup(xdg);
        } else if (home && *home) {
            base = bx_path_join(home, ".config");
        }
        if (base) {
            git_dir = bx_path_join(base, "git");
            if (git_dir)
                global_path = bx_path_join(git_dir, "ignore");
        }
        if (global_path) {
            bx_ignore_load_patterns_from_path(global_path, opts, &loaded, &loaded_n, false);
            if (loaded_n > 0) {
                chain = bx_ignore_append_state(chain, loaded, loaded_n, NULL, NULL,
                                               opts->ignore_file_case_insensitive);
                if (!chain) {
                    free(base);
                    free(git_dir);
                    free(global_path);
                    goto fail;
                }
            } else {
                bx_ignore_free_patterns(loaded, loaded_n);
            }
        }
        free(base);
        free(git_dir);
        free(global_path);
    }

    if (!opts->no_ignore && !opts->no_ignore_exclude && opts->git_root && opts->git_root[0] != '\0') {
        char *git_dir = bx_path_join(opts->git_root, ".git");
        char *info_dir = git_dir ? bx_path_join(git_dir, "info") : NULL;
        char *exclude_path = info_dir ? bx_path_join(info_dir, "exclude") : NULL;
        char **loaded = NULL;
        int loaded_n = 0;

        if (exclude_path) {
            bx_ignore_load_patterns_from_path(exclude_path, opts, &loaded, &loaded_n, false);
            if (loaded_n > 0) {
                chain = bx_ignore_append_state(chain, loaded, loaded_n, NULL, NULL,
                                               opts->ignore_file_case_insensitive);
                if (!chain) {
                    free(git_dir);
                    free(info_dir);
                    free(exclude_path);
                    goto fail;
                }
            } else {
                bx_ignore_free_patterns(loaded, loaded_n);
            }
        }
        free(git_dir);
        free(info_dir);
        free(exclude_path);
    }

    if (!root || !opts || opts->no_ignore || opts->no_ignore_parent)
        goto success;

    resolved_root = bx_path_realpath_dup(root);
    if (!resolved_root)
        goto success;

    char *cursor = bx_path_parent_dir_stripped_dup(resolved_root);
    if (!cursor) {
        free(resolved_root);
        resolved_root = NULL;
        goto fail;
    }

    char **dirs = NULL;
    int dir_count = 0;
    int dir_cap = 0;
    while (cursor && strcmp(cursor, "/") != 0) {
        if (dir_count >= dir_cap) {
            int new_cap = dir_cap == 0 ? 8 : dir_cap * 2;
            char **tmp = realloc(dirs, (size_t)new_cap * sizeof(*dirs));
            if (!tmp) {
                free(cursor);
                free(dirs);
                free(resolved_root);
                resolved_root = NULL;
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
            resolved_root = NULL;
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
            size_t dir_len = strlen(dirs[i]);
            const char *relative_root = resolved_root;
            if (strncmp(resolved_root, dirs[i], dir_len) == 0) {
                if (resolved_root[dir_len] == '/')
                    relative_root = resolved_root + dir_len + 1;
                else if (resolved_root[dir_len] == '\0')
                    relative_root = "";
            }
            chain = bx_ignore_append_state(chain, loaded, loaded_n, NULL, relative_root,
                                           opts->ignore_file_case_insensitive);
            if (!chain) {
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                free(resolved_root);
                resolved_root = NULL;
                goto fail;
            }
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
    free(resolved_root);
    bx_ignore_state_dispose_chain(chain);
    if (ok)
        *ok = false;
    return NULL;

success:
    if (ok)
        *ok = true;
    return chain;
}

bool bx_ignore_path_ignored(const char *name, char **patterns, int n) {
    return bx_ignore_match_patterns(name, NULL, patterns, n, false) == BX_IGNORE_EXCLUDE;
}

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state,
                                  const char *name,
                                  const char *path,
                                  const char *root_relative_path) {
    for (const struct bx_ignore_state *it = state; it; it = it->parent) {
        const char *relative_path = bx_ignore_state_relative_path(it, path, root_relative_path);
        char stack_path[BX_IGNORE_STACK_PATH_BUFSIZE];
        char *heap_path = NULL;
        if (it->root_prefix && it->root_prefix_len > 0u) {
            size_t prefix_len = it->root_prefix_len;
            size_t rel_len = relative_path ? strlen(relative_path) : 0;
            size_t total = prefix_len + (rel_len > 0 ? 1 + rel_len : 0) + 1;
            char *prefixed_path = total <= sizeof(stack_path)
                ? stack_path
                : malloc(total);
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
            if (prefixed_path != stack_path)
                heap_path = prefixed_path;
            relative_path = prefixed_path;
        }
        enum bx_ignore_match_result result =
            bx_ignore_match_patterns(name, relative_path, it->patterns, it->pattern_count,
                                     it->casefold);
        free(heap_path);
        if (result == BX_IGNORE_EXCLUDE)
            return true;
        if (result == BX_IGNORE_INCLUDE)
            return false;
    }
    return false;
}
