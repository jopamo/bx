#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "dev_counters.h"
#include "fswalk/walk.h"
#include "ignore.h"
#include "lib/path_ops.h"

#define BX_IGNORE_STACK_PATH_BUFSIZE 512u

struct bx_ignore_program_cache_key {
    uint64_t content_hash;
    bool casefold;
    char *content;
    size_t content_len;
};

struct bx_ignore_program_cache_entry {
    uint64_t content_hash;
    bool casefold;
    char *content;
    size_t content_len;
    struct bx_ignore_program *program;
};

struct bx_ignore_program_cache_snapshot {
    size_t len;
    struct bx_ignore_program_cache_entry entries[];
};

/*
 * Ignore-program compilation is cold control-plane work. Keep the published
 * cache as an immutable snapshot and replace it with a rebuilt candidate after
 * a miss instead of mutating shared cache nodes in place.
 */
static pthread_mutex_t bx_ignore_program_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t bx_ignore_program_cache_once = PTHREAD_ONCE_INIT;
static struct bx_ignore_program_cache_snapshot *bx_ignore_program_cache_snapshot = NULL;

static uint64_t bx_ignore_program_hash_bytes(uint64_t hash, const void *data, size_t len) {
    const unsigned char *bytes = data;

    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static void bx_ignore_program_cache_key_dispose(struct bx_ignore_program_cache_key *key) {
    if (!key)
        return;
    free(key->content);
    key->content = NULL;
    key->content_len = 0u;
    key->content_hash = 0u;
    key->casefold = false;
}

static bool bx_ignore_program_cache_build_key(struct bx_ignore_program_cache_key *key,
                                              char *const *patterns,
                                              const enum bx_ignore_source_kind *sources,
                                              int pattern_count,
                                              bool casefold) {
    size_t total_len = 0u;
    uint64_t hash = UINT64_C(14695981039346656037);

    if (!key)
        return false;
    memset(key, 0, sizeof(*key));
    if (!patterns || pattern_count <= 0)
        return false;

    for (int i = 0; i < pattern_count; ++i) {
        if (!patterns[i])
            continue;
        total_len += strlen(patterns[i]) + 2u;
    }
    if (total_len == 0u)
        return false;

    key->content = malloc(total_len);
    if (!key->content)
        return false;

    char *out = key->content;
    for (int i = 0; i < pattern_count; ++i) {
        size_t pattern_len;

        if (!patterns[i])
            continue;
        enum bx_ignore_source_kind source = sources
            ? sources[i]
            : BX_IGNORE_SOURCE_BUILTIN;
        unsigned char source_byte =
            source == BX_IGNORE_SOURCE_GITIGNORE || source == BX_IGNORE_SOURCE_DOTIGNORE
                ? (unsigned char)source
                : (unsigned char)BX_IGNORE_SOURCE_BUILTIN;
        *out++ = (char)source_byte;
        pattern_len = strlen(patterns[i]);
        memcpy(out, patterns[i], pattern_len);
        out += pattern_len;
        *out++ = '\n';
        hash = bx_ignore_program_hash_bytes(hash, &source_byte, sizeof(source_byte));
        hash = bx_ignore_program_hash_bytes(hash, patterns[i], pattern_len);
        hash = bx_ignore_program_hash_bytes(hash, "\n", 1u);
    }

    key->content_hash = bx_ignore_program_hash_bytes(hash, &casefold, sizeof(casefold));
    key->casefold = casefold;
    key->content_len = total_len;
    return true;
}

static struct bx_ignore_program *
bx_ignore_program_cache_lookup_locked(const struct bx_ignore_program_cache_key *key) {
    const struct bx_ignore_program_cache_snapshot *snapshot = bx_ignore_program_cache_snapshot;

    if (!key || !key->content || key->content_len == 0u)
        return NULL;
    if (!snapshot)
        return NULL;

    for (size_t i = 0u; i < snapshot->len; ++i) {
        const struct bx_ignore_program_cache_entry *it = &snapshot->entries[i];
        if (it->content_hash != key->content_hash || it->casefold != key->casefold ||
            it->content_len != key->content_len) {
            continue;
        }
        if (memcmp(it->content, key->content, key->content_len) == 0)
            return it->program;
    }
    return NULL;
}

static struct bx_ignore_program_cache_snapshot *
bx_ignore_program_cache_snapshot_new(size_t len) {
    if (len > (SIZE_MAX - sizeof(struct bx_ignore_program_cache_snapshot)) /
                  sizeof(struct bx_ignore_program_cache_entry)) {
        return NULL;
    }

    struct bx_ignore_program_cache_snapshot *snapshot =
        calloc(1u, sizeof(*snapshot) + len * sizeof(snapshot->entries[0]));
    if (!snapshot)
        return NULL;
    snapshot->len = len;
    return snapshot;
}

static struct bx_ignore_program_cache_snapshot *
bx_ignore_program_cache_snapshot_rebuild_with_entry(
    const struct bx_ignore_program_cache_snapshot *old_snapshot,
    const struct bx_ignore_program_cache_key *key,
    struct bx_ignore_program *program) {
    size_t old_len = old_snapshot ? old_snapshot->len : 0u;
    if (old_len == SIZE_MAX)
        return NULL;
    struct bx_ignore_program_cache_snapshot *new_snapshot =
        bx_ignore_program_cache_snapshot_new(old_len + 1u);
    if (!new_snapshot)
        return NULL;

    struct bx_ignore_program_cache_entry *entry = &new_snapshot->entries[0];
    entry->content_hash = key->content_hash;
    entry->casefold = key->casefold;
    entry->content = key->content;
    entry->content_len = key->content_len;
    entry->program = program;

    if (old_len > 0u) {
        memcpy(&new_snapshot->entries[1],
               old_snapshot->entries,
               old_len * sizeof(old_snapshot->entries[0]));
    }
    return new_snapshot;
}

static void bx_ignore_program_cache_snapshot_free_container(
    struct bx_ignore_program_cache_snapshot *snapshot) {
    free(snapshot);
}

static void bx_ignore_program_cache_snapshot_destroy(
    struct bx_ignore_program_cache_snapshot *snapshot) {
    if (!snapshot)
        return;
    for (size_t i = 0u; i < snapshot->len; ++i) {
        struct bx_ignore_program_cache_entry *entry = &snapshot->entries[i];
        bx_ignore_program_destroy_process_lifetime(entry->program);
        free(entry->content);
    }
    free(snapshot);
}

static void bx_ignore_program_cache_dispose(void) {
    struct bx_ignore_program_cache_snapshot *snapshot;

    pthread_mutex_lock(&bx_ignore_program_cache_lock);
    snapshot = bx_ignore_program_cache_snapshot;
    bx_ignore_program_cache_snapshot = NULL;
    pthread_mutex_unlock(&bx_ignore_program_cache_lock);

    bx_ignore_program_cache_snapshot_destroy(snapshot);
}

static void bx_ignore_program_cache_init(void) {
    (void)atexit(bx_ignore_program_cache_dispose);
}

static bool bx_ignore_program_cache_insert_locked(struct bx_ignore_program_cache_key *key,
                                                  struct bx_ignore_program *program) {
    struct bx_ignore_program_cache_snapshot *old_snapshot;
    struct bx_ignore_program_cache_snapshot *new_snapshot;

    if (!key || !key->content || key->content_len == 0u || !program)
        return false;

    old_snapshot = bx_ignore_program_cache_snapshot;
    new_snapshot =
        bx_ignore_program_cache_snapshot_rebuild_with_entry(old_snapshot, key, program);
    if (!new_snapshot)
        return false;

    bx_ignore_program_make_process_lifetime(program);
    bx_ignore_program_cache_snapshot = new_snapshot;
    bx_ignore_program_cache_snapshot_free_container(old_snapshot);

    key->content = NULL;
    key->content_len = 0u;
    return true;
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

enum bx_ignore_state_match_mode {
    BX_IGNORE_STATE_MATCH_LITERAL_BASENAME = 0,
    BX_IGNORE_STATE_MATCH_LITERAL_EXTENSION,
    BX_IGNORE_STATE_MATCH_LITERAL_DIRECTORY,
    BX_IGNORE_STATE_MATCH_ANCHORED_PREFIX,
    BX_IGNORE_STATE_MATCH_GENERIC_GLOB_FALLBACK,
    BX_IGNORE_STATE_MATCH_FULL,
};

static bool bx_ignore_append_pattern(char ***patterns,
                                     enum bx_ignore_source_kind **sources,
                                     int *n,
                                     int *cap,
                                     const char *pattern,
                                     enum bx_ignore_source_kind source) {
    if (*n >= *cap) {
        int new_cap = *cap == 0 ? 16 : *cap * 2;
        char **tmp = realloc(*patterns, (size_t)new_cap * sizeof(**patterns));
        if (!tmp)
            return false;
        enum bx_ignore_source_kind *source_tmp =
            realloc(*sources, (size_t)new_cap * sizeof(**sources));
        if (!source_tmp) {
            *patterns = tmp;
            return false;
        }
        *patterns = tmp;
        *sources = source_tmp;
        *cap = new_cap;
    }

    (*patterns)[*n] = strdup(pattern);
    if (!(*patterns)[*n])
        return false;
    (*sources)[*n] = source;
    (*n)++;
    return true;
}

static void bx_ignore_free_patterns(char **patterns,
                                    enum bx_ignore_source_kind *sources,
                                    int n) {
    if (!patterns)
        goto done;
    for (int i = 0; i < n; i++)
        free(patterns[i]);
    free(patterns);
done:
    free(sources);
}

void bx_ignore_state_init(struct bx_ignore_state *state,
                          const struct bx_ignore_state *parent,
                          const char *dirpath,
                          struct bx_ignore_program *program) {
    if (!state)
        return;
    state->parent = parent;
    state->dirpath = dirpath;
    state->owned_dirpath = NULL;
    state->dirpath_len = dirpath ? strlen(dirpath) : 0u;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->root_prefix_len = 0u;
    state->program = program;
    state->basename_only_chain =
        (!parent || parent->basename_only_chain) &&
        (!program || bx_ignore_program_is_basename_only(program));
    state->has_generic_glob_fallback_chain =
        (parent && parent->has_generic_glob_fallback_chain) ||
        (program && bx_ignore_program_has_generic_glob_fallback(program));
}

void bx_ignore_state_dispose(struct bx_ignore_state *state) {
    if (!state)
        return;
    bx_ignore_program_release(state->program);
    state->parent = NULL;
    free(state->owned_dirpath);
    free(state->owned_root_prefix);
    state->dirpath = NULL;
    state->owned_dirpath = NULL;
    state->dirpath_len = 0u;
    state->root_prefix = NULL;
    state->owned_root_prefix = NULL;
    state->root_prefix_len = 0u;
    state->program = NULL;
    state->basename_only_chain = false;
    state->has_generic_glob_fallback_chain = false;
}

void bx_ignore_state_dispose_chain(struct bx_ignore_state *state) {
    while (state) {
        union {
            const struct bx_ignore_state *const_parent;
            struct bx_ignore_state *mutable_parent;
        } parent = {.const_parent = state->parent};
        bx_ignore_state_dispose(state);
        free(state);
        state = parent.mutable_parent;
    }
}

struct bx_ignore_state *bx_ignore_state_clone_chain(const struct bx_ignore_state *state) {
    if (!state)
        return NULL;
    if (state->program && !bx_ignore_program_is_process_lifetime(state->program))
        return NULL;

    struct bx_ignore_state *parent = bx_ignore_state_clone_chain(state->parent);
    struct bx_ignore_state *copy = calloc(1u, sizeof(*copy));
    if (!copy) {
        bx_ignore_state_dispose_chain(parent);
        return NULL;
    }

    bx_ignore_state_init(copy, parent, state->dirpath, state->program);
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
    return copy;
}

static bool bx_ignore_state_rewrite_root_prefixes(struct bx_ignore_state *state,
                                                  const char *current_root,
                                                  const char *subtree_root) {
    if (!state)
        return true;
    union {
        const struct bx_ignore_state *const_parent;
        struct bx_ignore_state *mutable_parent;
    } parent = {.const_parent = state->parent};
    if (!bx_ignore_state_rewrite_root_prefixes(parent.mutable_parent,
                                              current_root,
                                              subtree_root))
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
                                              char ***patterns,
                                              enum bx_ignore_source_kind **sources,
                                              int *n,
                                              bool explicit_file,
                                              enum bx_ignore_source_kind source) {
    FILE *f;
    int cap = *n;
    bool loaded_any = false;

    if (!patterns || !sources || !n)
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
        if (!bx_ignore_append_pattern(patterns, sources, n, &cap, line, source)) {
            free(line);
            fclose(f);
            return loaded_any || *n > 0;
        }
    }
    free(line);
    fclose(f);
    return loaded_any && *n > 0;
}

static struct bx_ignore_program *bx_ignore_program_from_patterns(char **patterns,
                                                                 enum bx_ignore_source_kind *sources,
                                                                 int pattern_count,
                                                                 bool casefold) {
    struct bx_ignore_program_cache_key cache_key;
    struct bx_ignore_program *program = NULL;
    bool have_cache_key;

    have_cache_key = bx_ignore_program_cache_build_key(&cache_key,
                                                       patterns,
                                                       sources,
                                                       pattern_count,
                                                       casefold);
    if (have_cache_key) {
        pthread_once(&bx_ignore_program_cache_once, bx_ignore_program_cache_init);
        pthread_mutex_lock(&bx_ignore_program_cache_lock);
        program = bx_ignore_program_cache_lookup_locked(&cache_key);
        if (program) {
            pthread_mutex_unlock(&bx_ignore_program_cache_lock);
            bx_ignore_free_patterns(patterns, sources, pattern_count);
            bx_ignore_program_cache_key_dispose(&cache_key);
            return program;
        }
        pthread_mutex_unlock(&bx_ignore_program_cache_lock);
    }

    program = bx_ignore_program_compile_with_sources(patterns, sources, pattern_count, casefold);
    bx_ignore_free_patterns(patterns, sources, pattern_count);
    if (!program) {
        if (have_cache_key)
            bx_ignore_program_cache_key_dispose(&cache_key);
        return NULL;
    }

    if (have_cache_key) {
        pthread_mutex_lock(&bx_ignore_program_cache_lock);
        struct bx_ignore_program *cached = bx_ignore_program_cache_lookup_locked(&cache_key);
        if (cached) {
            pthread_mutex_unlock(&bx_ignore_program_cache_lock);
            bx_ignore_program_release(program);
            bx_ignore_program_cache_key_dispose(&cache_key);
            return cached;
        }
        if (bx_ignore_program_cache_insert_locked(&cache_key, program)) {
            pthread_mutex_unlock(&bx_ignore_program_cache_lock);
            bx_ignore_program_cache_key_dispose(&cache_key);
            return program;
        }
        pthread_mutex_unlock(&bx_ignore_program_cache_lock);
        bx_ignore_program_cache_key_dispose(&cache_key);
    }
    return program;
}

static struct bx_ignore_program *bx_ignore_load_program_from_path(const char *path,
                                                                  const struct bx_walk_ignore_opts *opts,
                                                                  bool explicit_file,
                                                                  enum bx_ignore_source_kind source,
                                                                  bool casefold) {
    char **patterns = NULL;
    enum bx_ignore_source_kind *sources = NULL;
    int pattern_count = 0;

    bx_ignore_load_patterns_from_path(path, opts, &patterns, &sources, &pattern_count,
                                      explicit_file, source);
    return bx_ignore_program_from_patterns(patterns, sources, pattern_count, casefold);
}

static struct bx_ignore_state *bx_ignore_append_state(struct bx_ignore_state *chain,
                                                      struct bx_ignore_program *program,
                                                      const char *dirpath,
                                                      const char *root_prefix) {
    struct bx_ignore_state *state;

    if (!program) {
        return chain;
    }

    state = calloc(1, sizeof(*state));
    if (!state) {
        bx_ignore_program_release(program);
        return NULL;
    }
    bx_ignore_state_init(state, chain, dirpath, program);
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
    return state;
}

struct bx_ignore_program *bx_ignore_load_program(const char *dirpath,
                                                 const struct bx_walk_ignore_opts *opts) {
    char **patterns = NULL;
    enum bx_ignore_source_kind *sources = NULL;
    int pattern_count = 0;

    if (!opts || !opts->ignore_filenames || opts->num_ignore_filenames <= 0)
        return NULL;

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
        enum bx_ignore_source_kind source = strcmp(filename, ".gitignore") == 0
            ? BX_IGNORE_SOURCE_GITIGNORE
            : strcmp(filename, ".ignore") == 0
                ? BX_IGNORE_SOURCE_DOTIGNORE
                : BX_IGNORE_SOURCE_BUILTIN;
        (void)bx_ignore_load_patterns_from_path(ignore_path, opts, &patterns, &sources,
                                                &pattern_count, false, source);
        free(ignore_path);
    }

    return bx_ignore_program_from_patterns(patterns, sources, pattern_count,
                                           opts->ignore_file_case_insensitive);
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
    bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_CALLS, 1u);
    bool found = lstat(git_path, &st) == 0;
    if (!found)
        bx_search_dev_counters_note_walk(BX_SEARCH_WALK_IGNORE_GIT_ROOT_LSTAT_MISSES, 1u);
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
            struct bx_ignore_program *program =
                bx_ignore_load_program_from_path(opts->extra_ignore_files[i], opts, false,
                                                 BX_IGNORE_SOURCE_BUILTIN,
                                                 opts->ignore_file_case_insensitive);
            if (program) {
                chain = bx_ignore_append_state(chain, program, NULL, NULL);
                if (!chain)
                    goto fail;
            }
        }
    }

    if (!opts->no_ignore && !opts->no_ignore_global && opts->gitignore_enabled) {
        const char *xdg = getenv("XDG_CONFIG_HOME");
        const char *home = getenv("HOME");
        char *base = NULL;
        char *git_dir = NULL;
        char *global_path = NULL;

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
            struct bx_ignore_program *program =
                bx_ignore_load_program_from_path(global_path, opts, false,
                                                 BX_IGNORE_SOURCE_GITIGNORE,
                                                 opts->ignore_file_case_insensitive);
            if (program) {
                chain = bx_ignore_append_state(chain, program, NULL, NULL);
                if (!chain) {
                    free(base);
                    free(git_dir);
                    free(global_path);
                    goto fail;
                }
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

        if (exclude_path) {
            struct bx_ignore_program *program =
                bx_ignore_load_program_from_path(exclude_path, opts, false,
                                                 BX_IGNORE_SOURCE_GITIGNORE,
                                                 opts->ignore_file_case_insensitive);
            if (program) {
                chain = bx_ignore_append_state(chain, program, NULL, NULL);
                if (!chain) {
                    free(git_dir);
                    free(info_dir);
                    free(exclude_path);
                    goto fail;
                }
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
        struct bx_ignore_program *program = bx_ignore_load_program(dirs[i], opts);
        if (program) {
            size_t dir_len = strlen(dirs[i]);
            const char *relative_root = resolved_root;
            if (strncmp(resolved_root, dirs[i], dir_len) == 0) {
                if (resolved_root[dir_len] == '/')
                    relative_root = resolved_root + dir_len + 1;
                else if (resolved_root[dir_len] == '\0')
                    relative_root = "";
            }
            chain = bx_ignore_append_state(chain, program, NULL, relative_root);
            if (!chain) {
                for (int k = 0; k < dir_count; k++)
                    free(dirs[k]);
                free(dirs);
                free(resolved_root);
                resolved_root = NULL;
                goto fail;
            }
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

static enum bx_ignore_match_result
bx_ignore_state_match_with_mode(const struct bx_ignore_state *state,
                                const char *name,
                                const char *path,
                                const char *root_relative_path,
                                bool is_dir,
                                enum bx_ignore_state_match_mode mode) {
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
            mode == BX_IGNORE_STATE_MATCH_LITERAL_BASENAME
                ? bx_ignore_program_match_literal_basename(it->program,
                                                           name,
                                                           relative_path,
                                                           is_dir)
            : mode == BX_IGNORE_STATE_MATCH_LITERAL_EXTENSION
                ? bx_ignore_program_match_literal_extension(it->program,
                                                            name,
                                                            relative_path,
                                                            is_dir)
            : mode == BX_IGNORE_STATE_MATCH_LITERAL_DIRECTORY
                ? bx_ignore_program_match_literal_directory(it->program,
                                                            name,
                                                            relative_path,
                                                            is_dir)
            : mode == BX_IGNORE_STATE_MATCH_ANCHORED_PREFIX
                ? bx_ignore_program_match_anchored_prefix(it->program,
                                                          name,
                                                          relative_path,
                                                          is_dir)
            : mode == BX_IGNORE_STATE_MATCH_GENERIC_GLOB_FALLBACK
                ? bx_ignore_program_match_generic_glob_fallback(it->program,
                                                                name,
                                                                relative_path,
                                                                is_dir)
                : bx_ignore_program_match(it->program, name, relative_path, is_dir);
        free(heap_path);
        if (result == BX_IGNORE_EXCLUDE || result == BX_IGNORE_INCLUDE)
            return result;
    }
    return BX_IGNORE_NO_MATCH;
}

enum bx_ignore_match_result
bx_ignore_state_match_literal_basename(const struct bx_ignore_state *state,
                                       const char *name,
                                       const char *path,
                                       const char *root_relative_path,
                                       bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_LITERAL_BASENAME);
}

enum bx_ignore_match_result
bx_ignore_state_match_literal_extension(const struct bx_ignore_state *state,
                                        const char *name,
                                        const char *path,
                                        const char *root_relative_path,
                                        bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_LITERAL_EXTENSION);
}

enum bx_ignore_match_result
bx_ignore_state_match_literal_directory(const struct bx_ignore_state *state,
                                        const char *name,
                                        const char *path,
                                        const char *root_relative_path,
                                        bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_LITERAL_DIRECTORY);
}

enum bx_ignore_match_result
bx_ignore_state_match_anchored_prefix(const struct bx_ignore_state *state,
                                      const char *name,
                                      const char *path,
                                      const char *root_relative_path,
                                      bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_ANCHORED_PREFIX);
}

enum bx_ignore_match_result
bx_ignore_state_match_generic_glob_fallback(const struct bx_ignore_state *state,
                                            const char *name,
                                            const char *path,
                                            const char *root_relative_path,
                                            bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_GENERIC_GLOB_FALLBACK);
}

bool bx_ignore_state_is_basename_only_chain(const struct bx_ignore_state *state) {
    return state && state->basename_only_chain;
}

bool bx_ignore_state_has_generic_glob_fallback_chain(const struct bx_ignore_state *state) {
    return state && state->has_generic_glob_fallback_chain;
}

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state,
                                  const char *name,
                                  const char *path,
                                  const char *root_relative_path,
                                  bool is_dir) {
    return bx_ignore_state_match_with_mode(state,
                                           name,
                                           path,
                                           root_relative_path,
                                           is_dir,
                                           BX_IGNORE_STATE_MATCH_FULL) == BX_IGNORE_EXCLUDE;
}
