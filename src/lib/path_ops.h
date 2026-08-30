#ifndef BX_COMMON_PATH_OPS_H
#define BX_COMMON_PATH_OPS_H

#include <stddef.h>
#include <stdbool.h>

struct bx_path_components {
    char** parts;
    size_t count;
    size_t cap;
};

void bx_path_components_push_dup(struct bx_path_components* components, const char* part);
void bx_path_components_pop(struct bx_path_components* components);
void bx_path_components_free(struct bx_path_components* components);
bool bx_path_components_shift(struct bx_path_components* components, char** part_out);
void bx_path_components_append_raw(struct bx_path_components* components, const char* path);
void bx_path_components_insert_raw_path(struct bx_path_components* components, size_t index, const char* path);
void bx_path_components_append_normalized(struct bx_path_components* components, const char* path);
void bx_path_components_append_normalized_part(struct bx_path_components* components, const char* part);
void bx_path_components_prepend_raw_path(struct bx_path_components* components, const char* path);
char* bx_path_components_to_absolute_path(const struct bx_path_components* components, size_t count);

char* bx_path_getcwd_dup(void);
char* bx_path_realpath_dup(const char* path);
char* bx_path_make_absolute_dup(const char* path);
char* bx_path_normalize_absolute_lexical_dup(const char* path);
char* bx_path_normalize_relative_lexical_dup(const char* path);
bool bx_path_is_within(const char* path, const char* base);
char* bx_path_relative_path_between(const char* from_abs, const char* to_abs);

char* bx_path_join(const char* left, const char* right);
char* bx_path_join_root_relative(const char* root, const char* path);
char* bx_path_strip_trailing_slashes_dup(const char* path);
const char* bx_path_strip_dot_slash_prefix_ptr(const char* path);
const char* bx_path_basename_ptr(const char* path);
const char* bx_path_extension_ptr(const char* path);
char* bx_path_basename_dup(const char* path);
char* bx_path_remove_last_extension_dup(const char* path);
char* bx_path_dirname_dup(const char* path);
char* bx_path_readlink_dup(const char* path);
char* bx_path_parent_dir_dup(const char* path);
char* bx_path_parent_dir_stripped_dup(const char* path);
char* bx_path_parents_layout_dup(const char* source_operand);
bool bx_path_is_dot_or_dotdot(const char* name);
bool bx_path_is_absolute(const char* path);
bool bx_path_has_parent_reference(const char* path);
char* bx_path_build_dest(const char* source_operand, const char* destination_root, bool destination_is_directory, bool parents);

#endif /* BX_COMMON_PATH_OPS_H */
