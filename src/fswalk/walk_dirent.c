#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "walk_internal.h"

static bool bx_walk_dirent_list_reserve(struct bx_walk_dirent_list *list, size_t needed) {
    if (list->cap >= needed)
        return true;

    size_t new_cap = list->cap == 0 ? 16u : list->cap * 2u;
    while (new_cap < needed)
        new_cap *= 2u;

    struct bx_walk_dirent_item *tmp = realloc(list->items, new_cap * sizeof(*list->items));
    if (!tmp)
        return false;

    list->items = tmp;
    list->cap = new_cap;
    return true;
}

static int bx_walk_dirent_item_compare(const void *left, const void *right) {
    const struct bx_walk_dirent_item *a = left;
    const struct bx_walk_dirent_item *b = right;
    return strcmp(a->name, b->name);
}

void bx_walk_dirent_list_free(struct bx_walk_dirent_list *list) {
    if (!list)
        return;

    for (size_t i = 0; i < list->len; i++)
        free(list->items[i].name);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int bx_walk_dirent_list_read_sorted(DIR *dir, struct bx_walk_dirent_list *list, int *err_out) {
    if (err_out)
        *err_out = 0;

    errno = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (!bx_walk_dirent_list_reserve(list, list->len + 1u)) {
            if (err_out)
                *err_out = ENOMEM;
            return -1;
        }

        list->items[list->len].name = strdup(ent->d_name);
        if (!list->items[list->len].name) {
            if (err_out)
                *err_out = ENOMEM;
            return -1;
        }
        list->items[list->len].d_type = ent->d_type;
        list->len++;
        errno = 0;
    }

    if (errno != 0) {
        if (err_out)
            *err_out = errno;
        return -1;
    }

    if (list->len > 1u)
        qsort(list->items, list->len, sizeof(*list->items), bx_walk_dirent_item_compare);

    return 0;
}
