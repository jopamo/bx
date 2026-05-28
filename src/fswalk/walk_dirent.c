#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "walk_internal.h"

#define BX_WALK_GETDENTS64_BUF_SIZE 32768u

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

static int bx_walk_dirent_iterate_readdir(DIR *dir,
                                          bx_walk_dirent_visit_fn visit,
                                          void *user,
                                          int *err_out) {
    errno = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        int visit_rc = visit(ent->d_name, ent->d_type, user);
        if (visit_rc != 0)
            return visit_rc;
        errno = 0;
    }

    if (errno != 0) {
        if (err_out)
            *err_out = errno;
        return -1;
    }

    return 0;
}

#if defined(__linux__) && defined(SYS_getdents64)
struct bx_walk_linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

enum {
    BX_WALK_DIRENT_ITERATE_FALLBACK = 2,
};

static int bx_walk_dirent_iterate_getdents64(DIR *dir,
                                             bx_walk_dirent_visit_fn visit,
                                             void *user,
                                             int *err_out) {
    int fd = dirfd(dir);
    if (fd < 0) {
        if (err_out)
            *err_out = errno != 0 ? errno : EBADF;
        return -1;
    }

    unsigned char buf[BX_WALK_GETDENTS64_BUF_SIZE];
    for (;;) {
        ssize_t nread = syscall(SYS_getdents64, fd, buf, sizeof(buf));
        if (nread == 0)
            return 0;
        if (nread < 0) {
            if (errno == ENOSYS)
                return BX_WALK_DIRENT_ITERATE_FALLBACK;
            if (err_out)
                *err_out = errno;
            return -1;
        }

        size_t pos = 0u;
        while (pos < (size_t)nread) {
            const struct bx_walk_linux_dirent64 *ent =
                (const struct bx_walk_linux_dirent64 *)(buf + pos);

            if (ent->d_reclen == 0u ||
                ent->d_reclen > ((size_t)nread - pos) ||
                ent->d_reclen < offsetof(struct bx_walk_linux_dirent64, d_name) + 1u) {
                if (err_out)
                    *err_out = EIO;
                return -1;
            }

            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                int visit_rc = visit(ent->d_name, ent->d_type, user);
                if (visit_rc != 0)
                    return visit_rc;
            }

            pos += ent->d_reclen;
        }
    }
}
#endif

struct bx_walk_dirent_list_collect_state {
    struct bx_walk_dirent_list *list;
};

static int bx_walk_dirent_list_collect(const char *name, unsigned char d_type, void *user) {
    struct bx_walk_dirent_list_collect_state *state = user;
    struct bx_walk_dirent_list *list = state ? state->list : NULL;

    if (!list)
        return -1;
    if (!bx_walk_dirent_list_reserve(list, list->len + 1u)) {
        errno = ENOMEM;
        return -1;
    }

    list->items[list->len].name = strdup(name);
    if (!list->items[list->len].name) {
        errno = ENOMEM;
        return -1;
    }
    list->items[list->len].d_type = d_type;
    list->len++;
    return 0;
}

void bx_walk_dirent_list_free(struct bx_walk_dirent_list *list) {
    if (!list)
        return;

    for (size_t i = 0; i < list->len; i++)
        free(list->items[i].name);
    free(list->items);
    memset(list, 0, sizeof(*list));
}

int bx_walk_dirent_iterate(DIR *dir,
                           bx_walk_dirent_visit_fn visit,
                           void *user,
                           int *err_out) {
    if (err_out)
        *err_out = 0;
    if (!dir || !visit) {
        if (err_out)
            *err_out = EINVAL;
        return -1;
    }

#if defined(__linux__) && defined(SYS_getdents64)
    int rc = bx_walk_dirent_iterate_getdents64(dir, visit, user, err_out);
    if (rc != BX_WALK_DIRENT_ITERATE_FALLBACK)
        return rc;
    rewinddir(dir);
#endif

    return bx_walk_dirent_iterate_readdir(dir, visit, user, err_out);
}

int bx_walk_dirent_list_read_sorted(DIR *dir, struct bx_walk_dirent_list *list, int *err_out) {
    struct bx_walk_dirent_list_collect_state state = {
        .list = list,
    };
    int rc = bx_walk_dirent_iterate(dir, bx_walk_dirent_list_collect, &state, err_out);
    if (rc != 0) {
        if (err_out && *err_out == 0)
            *err_out = errno != 0 ? errno : EIO;
        return -1;
    }

    if (list->len > 1u)
        qsort(list->items, list->len, sizeof(*list->items), bx_walk_dirent_item_compare);

    return 0;
}
