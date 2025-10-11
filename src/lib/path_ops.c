#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lib/path_ops.h"
#include "bx/libbx.h"

static char* bx_path_dup_range(const char* start, size_t len) {
    char* res = xmalloc(len + 1u);
    memcpy(res, start, len);
    res[len] = '\0';
    return res;
}

void bx_path_components_push_dup(struct bx_path_components* components, const char* part) {
    components->parts = xrealloc(components->parts, sizeof(*components->parts) * (components->count + 1u));
    components->parts[components->count++] = xstrdup(part);
}

void bx_path_components_pop(struct bx_path_components* components) {
    if (components->count == 0u) {
        return;
    }

    free(components->parts[components->count - 1u]);
    components->count--;
}

void bx_path_components_free(struct bx_path_components* components) {
    for (size_t i = 0; i < components->count; i++) {
        free(components->parts[i]);
    }

    free(components->parts);
    components->parts = NULL;
    components->count = 0u;
}

bool bx_path_components_shift(struct bx_path_components* components, char** part_out) {
    if (components->count == 0u) {
        return false;
    }

    char* part = components->parts[0];
    if (components->count > 1u) {
        memmove(components->parts, components->parts + 1u, (components->count - 1u) * sizeof(*components->parts));
    }
    components->count--;
    *part_out = part;
    return true;
}

void bx_path_components_append_raw(struct bx_path_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        bx_path_components_push_dup(components, token);
    }

    free(copy);
}

void bx_path_components_insert_raw_path(struct bx_path_components* components, size_t index, const char* path) {
    struct bx_path_components inserted = {0};

    bx_path_components_append_raw(&inserted, path);
    if (inserted.count == 0u) {
        return;
    }

    components->parts = xrealloc(components->parts, sizeof(*components->parts) * (components->count + inserted.count));
    memmove(&components->parts[index + inserted.count], &components->parts[index], sizeof(*components->parts) * (components->count - index));
    memcpy(&components->parts[index], inserted.parts, sizeof(*components->parts) * inserted.count);
    components->count += inserted.count;
    free(inserted.parts);
}

void bx_path_components_append_normalized_part(struct bx_path_components* components, const char* part) {
    if (strcmp(part, ".") == 0 || part[0] == '\0') {
        return;
    }
    if (strcmp(part, "..") == 0) {
        bx_path_components_pop(components);
        return;
    }

    bx_path_components_push_dup(components, part);
}

void bx_path_components_append_normalized(struct bx_path_components* components, const char* path) {
    char* copy = xstrdup(path);
    char* saveptr = NULL;

    for (char* token = strtok_r(copy, "/", &saveptr); token != NULL; token = strtok_r(NULL, "/", &saveptr)) {
        bx_path_components_append_normalized_part(components, token);
    }

    free(copy);
}

void bx_path_components_prepend_raw_path(struct bx_path_components* components, const char* path) {
    struct bx_path_components head = {0};
    bx_path_components_append_raw(&head, path);

    if (head.count == 0u) {
        bx_path_components_free(&head);
        return;
    }

    char** merged = xmalloc((head.count + components->count) * sizeof(*merged));
    memcpy(merged, head.parts, head.count * sizeof(*merged));
    if (components->count > 0u) {
        memcpy(merged + head.count, components->parts, components->count * sizeof(*merged));
    }

    free(head.parts);
    free(components->parts);
    components->parts = merged;
    components->count += head.count;
}

char* bx_path_components_to_absolute_path(const struct bx_path_components* components, size_t count) {
    if (count == 0u) {
        return xstrdup("/");
    }

    size_t len = 2u;
    for (size_t i = 0; i < count; i++) {
        len += strlen(components->parts[i]);
        if (i + 1u < count) {
            len++;
        }
    }

    char* path = xmalloc(len);
    size_t pos = 0u;
    path[pos++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t part_len = strlen(components->parts[i]);
        memcpy(path + pos, components->parts[i], part_len);
        pos += part_len;
        if (i + 1u < count) {
            path[pos++] = '/';
        }
    }
    path[pos] = '\0';
    return path;
}

char* bx_path_getcwd_dup(void) {
    size_t size = 128u;
    char* cwd = xmalloc(size);

    while (getcwd(cwd, size) == NULL) {
        if (errno != ERANGE) {
            free(cwd);
            return NULL;
        }
        size *= 2u;
        cwd = xrealloc(cwd, size);
    }

    return cwd;
}

char* bx_path_make_absolute_dup(const char* path) {
    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] == '/') {
        return xstrdup(path);
    }

    char* cwd = bx_path_getcwd_dup();
    if (cwd == NULL) {
        return NULL;
    }

    char* absolute = bx_path_join(cwd, path);
    free(cwd);
    return absolute;
}

char* bx_path_normalize_absolute_lexical_dup(const char* path) {
    struct bx_path_components components = {0};
    char* cwd = NULL;
    char* normalized = NULL;

    if (path == NULL) {
        errno = EINVAL;
        return NULL;
    }

    if (path[0] != '/') {
        cwd = bx_path_getcwd_dup();
        if (cwd == NULL) {
            return NULL;
        }
        bx_path_components_append_normalized(&components, cwd);
    }

    bx_path_components_append_normalized(&components, path);
    normalized = bx_path_components_to_absolute_path(&components, components.count);

    free(cwd);
    bx_path_components_free(&components);
    return normalized;
}

bool bx_path_is_within(const char* path, const char* base) {
    size_t base_len;

    if (path == NULL || base == NULL || path[0] != '/' || base[0] != '/') {
        return false;
    }

    if (strcmp(base, "/") == 0) {
        return true;
    }

    base_len = strlen(base);
    if (strncmp(path, base, base_len) != 0) {
        return false;
    }
    return path[base_len] == '\0' || path[base_len] == '/';
}

char* bx_path_relative_path_between(const char* from_abs, const char* to_abs) {
    struct bx_path_components from_components = {0};
    struct bx_path_components to_components = {0};
    char* relative = NULL;
    size_t common = 0u;

    if (from_abs == NULL || to_abs == NULL || from_abs[0] != '/' || to_abs[0] != '/') {
        errno = EINVAL;
        return NULL;
    }

    bx_path_components_append_normalized(&from_components, from_abs);
    bx_path_components_append_normalized(&to_components, to_abs);

    while (common < from_components.count && common < to_components.count && strcmp(from_components.parts[common], to_components.parts[common]) == 0) {
        common++;
    }

    size_t up_count = from_components.count - common;
    size_t down_count = to_components.count - common;
    size_t segment_count = up_count + down_count;
    if (segment_count == 0u) {
        relative = xstrdup(".");
        goto out;
    }

    size_t len = 1u;
    if (segment_count > 1u) {
        len += segment_count - 1u;
    }
    len += up_count * 2u;
    for (size_t i = common; i < to_components.count; i++) {
        len += strlen(to_components.parts[i]);
    }

    relative = xmalloc(len);
    size_t pos = 0u;
    for (size_t i = 0; i < up_count; i++) {
        if (pos > 0u) {
            relative[pos++] = '/';
        }
        relative[pos++] = '.';
        relative[pos++] = '.';
    }
    for (size_t i = common; i < to_components.count; i++) {
        if (pos > 0u) {
            relative[pos++] = '/';
        }
        size_t part_len = strlen(to_components.parts[i]);
        memcpy(relative + pos, to_components.parts[i], part_len);
        pos += part_len;
    }
    relative[pos] = '\0';

out:
    bx_path_components_free(&from_components);
    bx_path_components_free(&to_components);
    return relative;
}

char* bx_path_join(const char* left, const char* right) {
    size_t left_len = strlen(left);
    size_t right_len = strlen(right);
    bool need_slash = (left_len > 0 && left[left_len - 1] != '/');
    char* res = xmalloc(left_len + (need_slash ? 1u : 0u) + right_len + 1u);

    memcpy(res, left, left_len);
    size_t pos = left_len;
    if (need_slash) {
        res[pos++] = '/';
    }
    memcpy(res + pos, right, right_len);
    res[pos + right_len] = '\0';
    return res;
}

char* bx_path_strip_trailing_slashes_dup(const char* path) {
    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    return bx_path_dup_range(path, len);
}

const char* bx_path_basename_ptr(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

const char* bx_path_extension_ptr(const char* path) {
    const char* base = bx_path_basename_ptr(path);
    const char* dot = strrchr(base, '.');
    if (dot == NULL || dot == base) {
        return NULL;
    }
    return dot;
}

char* bx_path_basename_dup(const char* path) {
    if (path[0] == '\0') {
        return xstrdup("");
    }

    const char* end = path + strlen(path);
    while (end > path + 1 && end[-1] == '/') {
        end--;
    }

    const char* base = end;
    while (base > path && base[-1] != '/') {
        base--;
    }

    if (base == end) {
        return xstrdup("/");
    }
    return bx_path_dup_range(base, (size_t)(end - base));
}

char* bx_path_remove_last_extension_dup(const char* path) {
    const char* dot = bx_path_extension_ptr(path);
    if (dot == NULL) {
        return xstrdup(path);
    }
    return bx_path_dup_range(path, (size_t)(dot - path));
}

char* bx_path_readlink_dup(const char* path) {
    size_t cap = 128u;
    char* target = xmalloc(cap + 1u);

    for (;;) {
        ssize_t nread = readlink(path, target, cap);
        if (nread < 0) {
            free(target);
            return NULL;
        }
        if ((size_t)nread < cap) {
            target[nread] = '\0';
            return target;
        }

        if (cap > (SIZE_MAX / 2u) - 1u) {
            free(target);
            errno = ENOMEM;
            return NULL;
        }
        cap *= 2u;
        target = xrealloc(target, cap + 1u);
    }
}

char* bx_path_dirname_dup(const char* path) {
    if (path[0] == '\0') {
        return xstrdup(".");
    }

    size_t end = strlen(path);
    while (end > 0u && path[end - 1u] == '/') {
        end--;
    }

    if (end == 0u) {
        return xstrdup("/");
    }

    size_t slash_index = end;
    while (slash_index > 0u && path[slash_index - 1u] != '/') {
        slash_index--;
    }

    if (slash_index == 0u) {
        return xstrdup(".");
    }

    size_t dir_len = slash_index;
    while (dir_len > 1u && path[dir_len - 1u] == '/') {
        dir_len--;
    }

    return bx_path_dup_range(path, dir_len);
}

static char* bx_path_parent_dir_dup_impl(const char* path, bool strip_trailing) {
    char* copy = strip_trailing ? bx_path_strip_trailing_slashes_dup(path) : xstrdup(path);
    char* slash = strrchr(copy, '/');

    if (slash == NULL) {
        free(copy);
        return xstrdup(".");
    }
    if (slash == copy) {
        slash[1] = '\0';
        return copy;
    }

    *slash = '\0';
    return copy;
}

char* bx_path_parent_dir_dup(const char* path) {
    return bx_path_parent_dir_dup_impl(path, false);
}

char* bx_path_parent_dir_stripped_dup(const char* path) {
    return bx_path_parent_dir_dup_impl(path, true);
}

char* bx_path_parents_layout_dup(const char* source_operand) {
    const char* p = source_operand;
    char* copy;
    char* readp;
    char* writep;
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
        }
        else {
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

bool bx_path_is_dot_or_dotdot(const char* name) {
    return (strcmp(name, ".") == 0 || strcmp(name, "..") == 0);
}

char* bx_path_build_dest(const char* source_operand, const char* destination_root, bool destination_is_directory, bool parents) {
    if (parents) {
        char* parents_path = bx_path_parents_layout_dup(source_operand);
        char* res = bx_path_join(destination_root, parents_path);
        free(parents_path);
        return res;
    }

    if (destination_is_directory) {
        char* base = bx_path_basename_dup(source_operand);
        char* res = bx_path_join(destination_root, base);
        free(base);
        return res;
    }

    return xstrdup(destination_root);
}
