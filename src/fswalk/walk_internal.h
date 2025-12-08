#ifndef BX_FSWALK_WALK_INTERNAL_H
#define BX_FSWALK_WALK_INTERNAL_H

#include <dirent.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "walk.h"

struct bx_walk_ancestor {
    dev_t dev;
    ino_t ino;
    const char *path;
    const struct bx_walk_ancestor *parent;
};

struct bx_walk_dirent_item {
    char *name;
    unsigned char d_type;
};

struct bx_walk_dirent_list {
    struct bx_walk_dirent_item *items;
    size_t len;
    size_t cap;
};

struct bx_walk_ctx {
    const struct bx_walk_opts *opts;
    const struct bx_walk_ops *ops;
    void *user;
    dev_t root_device;
};

void bx_walk_entry_fill_from_stat(struct bx_walk_entry *entry, const struct stat *st);

void bx_walk_dirent_list_free(struct bx_walk_dirent_list *list);
int bx_walk_dirent_list_read_sorted(DIR *dir, struct bx_walk_dirent_list *list, int *err_out);

bool bx_walk_should_stop(const struct bx_walk_opts *opts);
const char *bx_walk_error_prefix(const struct bx_walk_opts *opts);
void bx_walk_report_error(const struct bx_walk_opts *opts, const char *path, int errnum);
void bx_walk_report_loop(const struct bx_walk_opts *opts, const char *path);

enum bx_walk_action bx_walk_handle_error(const struct bx_walk_ctx *ctx,
                                         const char *path,
                                         int errnum);
enum bx_walk_action bx_walk_apply_visit_action(const struct bx_walk_ctx *ctx,
                                               struct bx_walk_entry *entry,
                                               int *status_out);

bool bx_walk_ancestor_contains(const struct bx_walk_ancestor *anc, dev_t dev, ino_t ino);

char *bx_walk_path_join(const char *dirpath, const char *name, int *err_out);

#endif
