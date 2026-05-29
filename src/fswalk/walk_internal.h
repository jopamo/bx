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

typedef int (*bx_walk_dirent_visit_fn)(const char *name, unsigned char d_type, void *user);

struct bx_walk_path_buf;

struct bx_walk_ctx {
    const struct bx_walk_opts *opts;
    const struct bx_walk_ops *ops;
    void *user;
    dev_t root_device;
    struct bx_walk_path_buf *path_buf;
};

static inline void bx_walk_note_counter(const struct bx_walk_counter_ops *ops,
                                        enum bx_walk_counter counter,
                                        uint64_t count) {
    if (!ops || !ops->note || count == 0u)
        return;
    ops->note(counter, count, ops->user);
}

static inline void bx_walk_ctx_note_counter(const struct bx_walk_ctx *ctx,
                                            enum bx_walk_counter counter,
                                            uint64_t count) {
    if (!ctx || !ctx->opts)
        return;
    bx_walk_note_counter(ctx->opts->counter_ops, counter, count);
}

static inline void bx_walk_note_stat_call_for_reason(const struct bx_walk_counter_ops *ops,
                                                     enum bx_walk_counter syscall_counter,
                                                     enum bx_walk_counter reason_counter) {
    bx_walk_note_counter(ops, reason_counter, 1u);
    bx_walk_note_counter(ops, syscall_counter, 1u);
}

void bx_walk_entry_fill_from_stat(struct bx_walk_entry *entry, const struct stat *st);
int bx_walk_entry_fill_from_dirent(struct bx_walk_entry *entry,
                                   char *path,
                                   const char *name,
                                   unsigned char d_type,
                                   int parent_dirfd,
                                   bool follow_symlinks,
                                   int depth,
                                   const struct bx_walk_counter_ops *counter_ops,
                                   bool *entry_was_symlink);

int bx_walk_dirent_iterate(DIR *dir,
                           bx_walk_dirent_visit_fn visit,
                           void *user,
                           int *err_out,
                           const struct bx_walk_counter_ops *counter_ops);
void bx_walk_dirent_list_free(struct bx_walk_dirent_list *list);
int bx_walk_dirent_list_read_sorted(DIR *dir,
                                    struct bx_walk_dirent_list *list,
                                    int *err_out,
                                    const struct bx_walk_counter_ops *counter_ops);

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

char *bx_walk_path_join(const char *dirpath,
                        const char *name,
                        int *err_out,
                        const struct bx_walk_counter_ops *counter_ops);

#endif
