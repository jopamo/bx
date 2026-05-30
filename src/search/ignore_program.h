#ifndef BX_SEARCH_IGNORE_PROGRAM_H
#define BX_SEARCH_IGNORE_PROGRAM_H

#include <stdbool.h>

enum bx_ignore_match_result {
    BX_IGNORE_NO_MATCH = 0,
    BX_IGNORE_INCLUDE,
    BX_IGNORE_EXCLUDE,
};

enum bx_ignore_source_kind {
    BX_IGNORE_SOURCE_BUILTIN = 0,
    BX_IGNORE_SOURCE_GITIGNORE,
    BX_IGNORE_SOURCE_DOTIGNORE,
};

struct bx_ignore_program;

struct bx_ignore_program *bx_ignore_program_compile(char *const *patterns,
                                                    int pattern_count,
                                                    bool casefold);
struct bx_ignore_program *
bx_ignore_program_compile_with_sources(char *const *patterns,
                                       const enum bx_ignore_source_kind *sources,
                                       int pattern_count,
                                       bool casefold);
void bx_ignore_program_release(struct bx_ignore_program *program);
void bx_ignore_program_make_process_lifetime(struct bx_ignore_program *program);
bool bx_ignore_program_is_process_lifetime(const struct bx_ignore_program *program);
void bx_ignore_program_destroy_process_lifetime(struct bx_ignore_program *program);
bool bx_ignore_program_is_basename_only(const struct bx_ignore_program *program);
bool bx_ignore_program_has_generic_glob_fallback(const struct bx_ignore_program *program);

enum bx_ignore_match_result
bx_ignore_program_match_literal_basename(const struct bx_ignore_program *program,
                                         const char *name,
                                         const char *relative_path,
                                         bool is_dir);

enum bx_ignore_match_result
bx_ignore_program_match_literal_extension(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir);

enum bx_ignore_match_result
bx_ignore_program_match_literal_directory(const struct bx_ignore_program *program,
                                          const char *name,
                                          const char *relative_path,
                                          bool is_dir);

enum bx_ignore_match_result
bx_ignore_program_match_anchored_prefix(const struct bx_ignore_program *program,
                                        const char *name,
                                        const char *relative_path,
                                        bool is_dir);

enum bx_ignore_match_result
bx_ignore_program_match_generic_glob_fallback(const struct bx_ignore_program *program,
                                              const char *name,
                                              const char *relative_path,
                                              bool is_dir);

enum bx_ignore_match_result bx_ignore_program_match(const struct bx_ignore_program *program,
                                                    const char *name,
                                                    const char *relative_path,
                                                    bool is_dir);

#endif
