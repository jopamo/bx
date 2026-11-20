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
};

bool bx_module_name_valid(const char* name);
bool bx_module_name_matches(const char* name, const char* key, size_t len);
bool bx_module_release_valid(const char* release);
char* bx_module_read_regular(int fd, size_t limit, size_t* size);

/* Initialize with .fd = -1 and close even on failure. A null release uses uname. */
int bx_module_tree_open(struct bx_module_tree* tree, const char* base, const char* release);
void bx_module_tree_close(struct bx_module_tree* tree);
/* Return 0 with a relative path, 1 for a built-in, or -1 on error. */
int bx_module_tree_find(struct bx_module_tree* tree, const char* name, char* path, size_t size);
int bx_module_tree_open_file(const struct bx_module_tree* tree, const char* path);
/* Return borrowed, validated module.key=value records (NUL-delimited). */
int bx_module_tree_builtin_info(struct bx_module_tree* tree, const char** data, size_t* size);
#endif

#endif
