#define _GNU_SOURCE
#include "module_plan.h"

#ifdef __linux__
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"

enum { MODULE_RECORD_LIMIT = 262144 };
enum visit { UNSEEN, ACTIVE, DONE };

struct entry {
    struct bx_module_dep record;
    struct entry* parent;
    enum visit visit;
};

static int compare_paths(struct bx_module_path a, struct bx_module_path b) {
    int r = memcmp(a.text, b.text, a.len < b.len ? a.len : b.len);
    return r ? r : (a.len > b.len) - (a.len < b.len);
}

static int compare_entries(const void* left, const void* right) {
    const struct entry* a = left;
    const struct entry* b = right;
    return compare_paths(a->record.path, b->record.path);
}

static struct entry* find_entry(struct entry* entries, size_t count, struct bx_module_path key) {
    struct entry search = {.record.path = key};
    return bsearch(&search, entries, count, sizeof(*entries), compare_entries);
}

static struct entry* read_entries(struct bx_module_tree* tree, size_t* count) {
    struct bx_module_dep_iter iter;
    if (bx_module_tree_deps(tree, &iter) < 0)
        return NULL;
    struct bx_module_dep_iter first = iter;
    struct bx_module_dep record;
    size_t n = 0;
    int r;
    while ((r = bx_module_dep_next(&iter, &record)) > 0) {
        if (n == MODULE_RECORD_LIMIT) {
            errno = EFBIG;
            return NULL;
        }
        if (record.path.len >= PATH_MAX || !bx_module_path_valid(record.path.text, record.path.len) ||
            bx_module_path_name(record.path, true).len == 0) {
            errno = EINVAL;
            return NULL;
        }
        n++;
    }
    if (r < 0)
        return NULL;
    struct entry* entries = calloc(n ? n : 1, sizeof(*entries));
    if (entries == NULL)
        return NULL;
    for (size_t i = 0; i < n; i++)
        bx_module_dep_next(&first, &entries[i].record);
    qsort(entries, n, sizeof(*entries), compare_entries);
    for (size_t i = 1; i < n; i++) {
        if (compare_entries(&entries[i - 1], &entries[i]) == 0) {
            free(entries);
            errno = EINVAL;
            return NULL;
        }
    }
    *count = n;
    return entries;
}

static bool space(char c) {
    return c == ' ' || c == '\t';
}

/* depmod lists dependents first; consume the list in reverse order. */
static struct bx_module_path next_dependency(struct bx_module_dep* record) {
    const char* data = record->dependencies;
    size_t end = record->length;
    while (end > 0 && space(data[end - 1]))
        end--;
    size_t start = end;
    while (start > 0 && !space(data[start - 1]))
        start--;
    record->length = start;
    return (struct bx_module_path) {data + start, end - start};
}

static int check_module(const struct bx_module_tree* tree, struct bx_module_path path) {
    char text[PATH_MAX];
    memcpy(text, path.text, path.len);
    text[path.len] = '\0';
    int fd = bx_module_tree_open_file(tree, text, O_PATH);
    if (fd < 0)
        return -1;
    struct stat st;
    int r = fstat(fd, &st);
    int error = r < 0 ? errno : S_ISREG(st.st_mode) ? 0 : EINVAL;
    bx_fd_cleanup(&fd);
    if (error) {
        errno = error;
        return -1;
    }
    return 0;
}

static int order_entries(const struct bx_module_tree* tree, struct entry* entries, size_t count,
                         struct entry* current, struct bx_module_plan* plan) {
    if (current == NULL) {
        errno = ENOENT;
        return -1;
    }
    current->visit = ACTIVE;
    while (current) {
        struct bx_module_path key = next_dependency(&current->record);
        if (key.len == 0) {
            if (check_module(tree, current->record.path) < 0)
                return -1;
            plan->files[plan->count++] = current->record.path;
            current->visit = DONE;
            current = current->parent;
            continue;
        }
        struct entry* dep = find_entry(entries, count, key);
        if (dep == NULL) {
            errno = ENOENT;
            return -1;
        }
        if (dep->visit == ACTIVE)
            return BX_MODULE_PLAN_CYCLE;
        if (dep->visit == UNSEEN) {
            dep->visit = ACTIVE;
            dep->parent = current;
            current = dep;
        }
    }
    return 0;
}

int bx_module_tree_plan(struct bx_module_tree* tree, const char* name, struct bx_module_plan* plan) {
    char path[PATH_MAX];
    int found = bx_module_tree_find(tree, name, path, sizeof(path));
    if (found < 0)
        return -1;
    if (found == 1) {
        *plan = (struct bx_module_plan) {.builtin = true};
        return 0;
    }
    size_t count;
    struct entry* entries = read_entries(tree, &count);
    if (entries == NULL)
        return -1;
    struct bx_module_plan result = {.files = calloc(count ? count : 1, sizeof(*result.files))};
    struct entry* root = find_entry(entries, count, (struct bx_module_path) {path, strlen(path)});
    int r = result.files ? order_entries(tree, entries, count, root, &result) : -1;
    int error = errno;
    free(entries);
    if (r < 0) {
        free(result.files);
        errno = error;
        return r;
    }
    *plan = result;
    return 0;
}
#endif
