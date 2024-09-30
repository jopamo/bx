#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "common/path_ops.h"
#include "libbx.h"

static char *bx_path_dup_range(const char *start, size_t len) {
    char *res = xmalloc(len + 1u);
    memcpy(res, start, len);
    res[len] = '\0';
    return res;
}

char *bx_path_join(const char *left, const char *right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool need_slash = (left_len > 0 && left[left_len - 1] != '/');
    char *res = xmalloc(left_len + (need_slash ? 1u : 0u) + right_len + 1u);

    memcpy(res, left, left_len);
    size_t pos = left_len;
    if (need_slash) {
        res[pos++] = '/';
    }
    memcpy(res + pos, right, right_len);
    res[pos + right_len] = '\0';
    return res;
}

char *bx_path_strip_trailing_slashes_dup(const char *path) {
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    return bx_path_dup_range(path, len);
}

char *bx_path_basename_dup(const char *path) {
    const char *end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }

    const char *base = end;
    while (base > path && base[-1] != '/') {
        base--;
    }

    if (base == end) {
        return xstrdup("/");
    }
    return bx_path_dup_range(base, (size_t)(end - base));
}

char *bx_path_parents_layout_dup(const char *source_operand) {
    const char *p = source_operand;
    char *copy;
    char *readp;
    char *writep;
    bool previous_was_slash = false;

    while (*p == '/') {
        p++;
    }

    copy = xstrdup(p);
    readp = copy;
    writep = copy;

    while (*readp != '\0') {
        if (readp[0] == '.' && (readp[1] == '/' || readp[1] == '\0')) {
            if (readp[1] == '/') {
                readp += 2;
                continue;
            }
            readp += 1;
            continue;
        }
        if (*readp == '/') {
            if (!previous_was_slash) {
                *writep++ = *readp;
            }
            previous_was_slash = true;
        } else {
            *writep++ = *readp;
            previous_was_slash = false;
        }
        readp++;
    }
    while (writep > copy + 1 && writep[-1] == '/') {
        writep--;
    }
    *writep = '\0';

    if (copy[0] == '\0') {
        free(copy);
        return xstrdup(".");
    }
    return copy;
}

bool bx_path_is_dot_or_dotdot(const char *name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

char *bx_path_build_dest(const char *source_operand,
                         const char *destination_root,
                         bool destination_is_directory,
                         bool parents) {
    if (parents) {
        char *parents_path = bx_path_parents_layout_dup(source_operand);
        char *res = bx_path_join(destination_root, parents_path);
        free(parents_path);
        return res;
    }

    if (destination_is_directory) {
        char *base = bx_path_basename_dup(source_operand);
        char *res = bx_path_join(destination_root, base);
        free(base);
        return res;
    }

    return xstrdup(destination_root);
}
