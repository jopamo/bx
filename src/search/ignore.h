#ifndef BX_SEARCH_IGNORE_H
#define BX_SEARCH_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

#include "fswalk/walk.h"
#include "ignore_program.h"

struct bx_ignore_state {
    const struct bx_ignore_state *parent;
    const char *dirpath;
    char *owned_dirpath;
    size_t dirpath_len;
    const char *root_prefix;
    char *owned_root_prefix;
    size_t root_prefix_len;
    struct bx_ignore_program *program;
    bool basename_only_chain;
    bool has_generic_glob_fallback_chain;
};

void bx_ignore_state_init(struct bx_ignore_state *state,
                          const struct bx_ignore_state *parent,
                          const char *dirpath,
                          struct bx_ignore_program *program);

void bx_ignore_state_dispose(struct bx_ignore_state *state);

void bx_ignore_state_dispose_chain(struct bx_ignore_state *state);
struct bx_ignore_state *bx_ignore_state_clone_chain(const struct bx_ignore_state *state);
struct bx_ignore_state *bx_ignore_state_clone_chain_for_subtree(const struct bx_ignore_state *state,
                                                                const char *current_root,
                                                                const char *subtree_root);

struct bx_ignore_program *bx_ignore_load_program(const char *dirpath,
                                                 const struct bx_walk_ignore_opts *opts);
void bx_ignore_validate_explicit_ignore_files(const struct bx_walk_ignore_opts *opts);

struct bx_ignore_state *bx_ignore_load_parent_state(const char *root,
                                                    const struct bx_walk_ignore_opts *opts,
                                                    bool *ok);

enum bx_ignore_match_result
bx_ignore_state_match_literal_basename(const struct bx_ignore_state *state,
                                       const char *name,
                                       const char *path,
                                       const char *root_relative_path,
                                       bool is_dir);

enum bx_ignore_match_result
bx_ignore_state_match_literal_extension(const struct bx_ignore_state *state,
                                        const char *name,
                                        const char *path,
                                        const char *root_relative_path,
                                        bool is_dir);

enum bx_ignore_match_result
bx_ignore_state_match_literal_directory(const struct bx_ignore_state *state,
                                        const char *name,
                                        const char *path,
                                        const char *root_relative_path,
                                        bool is_dir);

enum bx_ignore_match_result
bx_ignore_state_match_anchored_prefix(const struct bx_ignore_state *state,
                                      const char *name,
                                      const char *path,
                                      const char *root_relative_path,
                                      bool is_dir);

enum bx_ignore_match_result
bx_ignore_state_match_generic_glob_fallback(const struct bx_ignore_state *state,
                                            const char *name,
                                            const char *path,
                                            const char *root_relative_path,
                                            bool is_dir);

bool bx_ignore_state_is_basename_only_chain(const struct bx_ignore_state *state);
bool bx_ignore_state_has_generic_glob_fallback_chain(const struct bx_ignore_state *state);

bool bx_ignore_state_matches_path(const struct bx_ignore_state *state,
                                  const char *name,
                                  const char *path,
                                  const char *root_relative_path,
                                  bool is_dir);

bool bx_ignore_enable_gitignore_for_root(const char *root, const struct bx_walk_ignore_opts *opts);
char *bx_ignore_find_git_root(const char *root, const struct bx_walk_ignore_opts *opts);

#endif
