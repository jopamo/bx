#ifndef BX_MODULE_INDEX_H
#define BX_MODULE_INDEX_H

#ifdef __linux__
#include <stdbool.h>
#include <stddef.h>

struct bx_module_index {
    char* data;
    size_t size;
    int error;
    bool loaded;
};

struct bx_module_tree {
    int fd;
    char* path;
    struct bx_module_index deps;
    struct bx_module_index builtins;
    struct bx_module_index info;
    struct bx_module_index aliases;
};

struct bx_module_path {
    const char* text;
    size_t len;
};

struct bx_module_dep {
    struct bx_module_path path;
    const char* dependencies;
    size_t length;
};

struct bx_module_dep_iter {
    const char* next;
    const char* end;
};

bool bx_module_name_valid(const char* name);
bool bx_module_name_matches(const char* name, const char* key, size_t len);
bool bx_module_name_span_valid(const char* name, size_t len);
bool bx_module_release_valid(const char* release);
char* bx_module_read_regular(int fd, size_t limit, size_t* size);

/* Initialize with .fd = -1 and close even on failure. A null release uses uname. */
int bx_module_tree_open(struct bx_module_tree* tree, const char* base, const char* release);
void bx_module_tree_close(struct bx_module_tree* tree);
/* Return 0 with a relative path, 1 for a built-in, or -1 on error. */
int bx_module_tree_find(struct bx_module_tree* tree, const char* name, char* path, size_t size);
/* Return 1 if found, 0 if absent, or -1 on error. Path output is optional. */
int bx_module_tree_find_loadable(struct bx_module_tree* tree, const char* name, char* path, size_t size);
int bx_module_tree_has_builtin(struct bx_module_tree* tree, const char* name);
int bx_module_tree_open_file(const struct bx_module_tree* tree, const char* path, int flags);
bool bx_module_path_valid(const char* path, size_t len);
struct bx_module_path bx_module_path_name(struct bx_module_path path, bool compressed);
/* Dependency records borrow their text from the tree. next returns 1, 0 at EOF, or -1. */
int bx_module_tree_deps(struct bx_module_tree* tree, struct bx_module_dep_iter* iter);
int bx_module_dep_next(struct bx_module_dep_iter* iter, struct bx_module_dep* record);
/* Return borrowed, validated module.key=value records (NUL-delimited). */
int bx_module_tree_builtin_info(struct bx_module_tree* tree, const char** data, size_t* size);
int bx_module_tree_aliases(struct bx_module_tree* tree, const char** data, size_t* size);
#endif

#endif
