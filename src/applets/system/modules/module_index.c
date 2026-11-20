#define _GNU_SOURCE
#include "module_index.h"

#ifdef __linux__
#include <limits.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

#include "lib/fd_ops.h"
#include "lib/open_resolve.h"
#include "lib/xreadwrite.h"

enum { MODULE_INDEX_LIMIT = 32 * 1024 * 1024 };

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool bx_module_name_valid(const char* name) {
    return bx_module_name_span_valid(name, strlen(name));
}

bool bx_module_name_span_valid(const char* name, size_t len) {
    if (len == 0 || len > NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!name_char(name[i]))
            return false;
    }
    return true;
}

bool bx_module_name_matches(const char* name, const char* key, size_t len) {
    if (strlen(name) != len)
        return false;
    for (size_t i = 0; i < len; i++) {
        if ((name[i] == '-' ? '_' : name[i]) != (key[i] == '-' ? '_' : key[i]))
            return false;
    }
    return true;
}

bool bx_module_release_valid(const char* release) {
    size_t len = strlen(release);
    if (len == 0 || len > NAME_MAX || strcmp(release, ".") == 0 || strcmp(release, "..") == 0)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!name_char(release[i]) && release[i] != '.' && release[i] != '+')
            return false;
    }
    return true;
}

char* bx_module_read_regular(int fd, size_t limit, size_t* size) {
    struct stat st;
    if (fstat(fd, &st) < 0)
        return NULL;
    if (!S_ISREG(st.st_mode) || st.st_size < 0) {
        errno = EINVAL;
        return NULL;
    }
    if ((uintmax_t)st.st_size > limit) {
        errno = EFBIG;
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char* data = malloc(len == 0 ? 1 : len);
    if (data == NULL)
        return NULL;
    for (size_t pos = 0; pos < len;) {
        ssize_t n = bx_xread(fd, data + pos, len - pos);
        if (n <= 0) {
            int error = n == 0 ? EIO : errno;
            free(data);
            errno = error;
            return NULL;
        }
        pos += (size_t)n;
    }
    *size = len;
    return data;
}

int bx_module_tree_open(struct bx_module_tree* tree, const char* base, const char* release) {
    struct utsname uts;
    if (release == NULL) {
        if (uname(&uts) < 0)
            return -1;
        release = uts.release;
    }
    if (!bx_module_release_valid(release)) {
        errno = EINVAL;
        return -1;
    }
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/')
        len--;
    if (len > INT_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (asprintf(&tree->path, "%.*s/lib/modules/%s", (int)len, base, release) < 0) {
        tree->path = NULL;
        return -1;
    }
    tree->fd = bx_fd_open_cloexec(tree->path, O_RDONLY | O_DIRECTORY, 0);
    return tree->fd < 0 ? -1 : 0;
}

void bx_module_tree_close(struct bx_module_tree* tree) {
    bx_fd_cleanup(&tree->fd);
    free(tree->path);
    free(tree->deps.data);
    free(tree->builtins.data);
    free(tree->info.data);
    free(tree->aliases.data);
}

int bx_module_tree_open_file(const struct bx_module_tree* tree, const char* path, int flags) {
    return bx_openat_resolve(tree->fd, path, flags | O_CLOEXEC, 0,
                            RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS);
}

static bool builtin_info_valid(const char* data, size_t len) {
    while (len != 0) {
        const char* end = memchr(data, '\0', len);
        if (end == NULL)
            return false;
        if (end != data) {
            const char* eq = memchr(data, '=', (size_t)(end - data));
            const char* dot = eq ? memchr(data, '.', (size_t)(eq - data)) : NULL;
            if (dot == NULL || dot == data || dot + 1 == eq)
                return false;
            for (const char* p = dot + 1; p < eq; p++) {
                if (!name_char(*p) || *p == '-')
                    return false;
            }
        }
        len -= (size_t)(end - data) + 1;
        data = end + 1;
    }
    return true;
}

static int load_index(const struct bx_module_tree* tree, struct bx_module_index* index,
                      const char* filename, bool builtin_info) {
    if (!index->loaded) {
        index->loaded = true;
        int fd = bx_module_tree_open_file(tree, filename, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            index->error = errno;
        } else {
            index->data = bx_module_read_regular(fd, MODULE_INDEX_LIMIT, &index->size);
            if (index->data == NULL) {
                index->error = errno;
            } else if (builtin_info ? !builtin_info_valid(index->data, index->size)
                                   : memchr(index->data, '\0', index->size) != NULL) {
                index->error = EINVAL;
            }
            bx_fd_cleanup(&fd);
        }
    }
    if (index->error == ENOENT)
        return 0; /* A tree can consist entirely of built-in or loadable modules. */
    if (index->error != 0) {
        errno = index->error;
        return -1;
    }
    return 1;
}

bool bx_module_path_valid(const char* path, size_t len) {
    size_t start = 0;
    for (size_t i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {
            if (i == start || (i - start == 1 && path[start] == '.') ||
                (i - start == 2 && path[start] == '.' && path[start + 1] == '.'))
                return false;
            start = i + 1;
        } else if ((unsigned char)path[i] <= ' ' || path[i] == 127 || path[i] == ':') {
            return false;
        }
    }
    return true;
}

int bx_module_tree_deps(struct bx_module_tree* tree, struct bx_module_dep_iter* iter) {
    int result = load_index(tree, &tree->deps, "modules.dep", false);
    if (result < 0)
        return -1;
    iter->next = result == 0 ? "" : tree->deps.data;
    iter->end = iter->next + tree->deps.size;
    return 0;
}

static bool next_line(struct bx_module_dep_iter* iter, const char** line, const char** end) {
    while (iter->next != iter->end) {
        const char* p = iter->next;
        const char* nl = memchr(p, '\n', (size_t)(iter->end - p));
        const char* stop = nl ? nl : iter->end;
        iter->next = nl ? nl + 1 : stop;
        if (p != stop) {
            *line = p;
            *end = stop;
            return true;
        }
    }
    return false;
}

int bx_module_dep_next(struct bx_module_dep_iter* iter, struct bx_module_dep* record) {
    const char* p;
    const char* end;
    if (!next_line(iter, &p, &end))
        return 0;
    const char* colon = memchr(p, ':', (size_t)(end - p));
    if (colon == NULL || colon == p) {
        errno = EINVAL;
        return -1;
    }
    *record = (struct bx_module_dep) {
        .path = {p, (size_t)(colon - p)},
        .dependencies = colon + 1,
        .length = (size_t)(end - colon - 1),
    };
    return 1;
}

struct bx_module_path bx_module_path_name(struct bx_module_path path, bool compressed) {
    const char* file = path.text;
    for (size_t i = 0; i < path.len; i++) {
        if (path.text[i] == '/')
            file = path.text + i + 1;
    }
    size_t file_len = path.len - (size_t)(file - path.text);
    const char* suffix = memchr(file, '.', file_len);
    if (suffix != NULL) {
        size_t len = (size_t)(suffix - file), suffix_len = file_len - len;
        if ((suffix_len == 3 && memcmp(suffix, ".ko", 3) == 0) ||
            (compressed && ((suffix_len == 6 && (memcmp(suffix, ".ko.gz", 6) == 0 ||
                                                 memcmp(suffix, ".ko.xz", 6) == 0)) ||
                            (suffix_len == 7 && memcmp(suffix, ".ko.zst", 7) == 0)))) {
            if (bx_module_name_span_valid(file, len))
                return (struct bx_module_path) {file, len};
        }
    }
    return (struct bx_module_path) {0};
}

static bool module_path_matches(const char* name, struct bx_module_path path, bool compressed) {
    struct bx_module_path key = bx_module_path_name(path, compressed);
    return key.len != 0 && bx_module_name_matches(name, key.text, key.len);
}

int bx_module_tree_find_loadable(struct bx_module_tree* tree, const char* name, char* path, size_t size) {
    struct bx_module_dep_iter iter;
    if (bx_module_tree_deps(tree, &iter) < 0)
        return -1;
    int r;
    struct bx_module_dep record;
    while ((r = bx_module_dep_next(&iter, &record)) > 0) {
        const char* p = record.path.text;
        size_t len = record.path.len;
        if (!module_path_matches(name, record.path, true))
            continue;
        if (!bx_module_path_valid(p, len)) {
            errno = EINVAL;
            return -1;
        }
        if (path != NULL) {
            if (len >= size) {
                errno = ENAMETOOLONG;
                return -1;
            }
            memcpy(path, p, len);
            path[len] = '\0';
        }
        return 1;
    }
    return r;
}

int bx_module_tree_has_builtin(struct bx_module_tree* tree, const char* name) {
    int r = load_index(tree, &tree->builtins, "modules.builtin", false);
    if (r < 0)
        return -1;
    if (r > 0) {
        const char* data = tree->builtins.data;
        struct bx_module_dep_iter iter = {data, data + tree->builtins.size};
        const char* p;
        const char* end;
        while (next_line(&iter, &p, &end)) {
            size_t len = (size_t)(end - p);
            if (module_path_matches(name, (struct bx_module_path) {p, len}, false)) {
                if (bx_module_path_valid(p, len))
                    return 1;
                errno = EINVAL;
                return -1;
            }
        }
    }
    return 0;
}

int bx_module_tree_find(struct bx_module_tree* tree, const char* name, char* path, size_t size) {
    int r = bx_module_tree_find_loadable(tree, name, path, size);
    if (r != 0)
        return r > 0 ? 0 : -1;
    r = bx_module_tree_has_builtin(tree, name);
    if (r != 0)
        return r;
    errno = ENOENT;
    return -1;
}

static int index_contents(const struct bx_module_tree* tree, struct bx_module_index* index,
                          const char* filename, bool builtin_info, const char** data, size_t* size) {
    int r = load_index(tree, index, filename, builtin_info);
    if (r < 0)
        return -1;
    *data = r == 0 ? "" : index->data;
    *size = r == 0 ? 0 : index->size;
    return 0;
}

int bx_module_tree_builtin_info(struct bx_module_tree* tree, const char** data, size_t* size) {
    return index_contents(tree, &tree->info, "modules.builtin.modinfo", true, data, size);
}

int bx_module_tree_aliases(struct bx_module_tree* tree, const char** data, size_t* size) {
    return index_contents(tree, &tree->aliases, "modules.alias", false, data, size);
}
#endif
