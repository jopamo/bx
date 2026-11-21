#ifndef BX_RSYNC_METADATA_H
#define BX_RSYNC_METADATA_H

#include <stddef.h>
#include <stdint.h>

struct bx_rsync_meta;
struct sess;
struct flist;
struct ident;

int bx_rsync_meta_negotiate(struct sess *, int, int);
int bx_rsync_meta_prepare(struct sess *, const struct flist *, struct bx_rsync_meta **);
void bx_rsync_meta_free(struct bx_rsync_meta *);
size_t bx_rsync_meta_count(const struct bx_rsync_meta *);
int bx_rsync_meta_send(struct sess *, int, const struct bx_rsync_meta *,
    struct ident **, size_t *, struct ident **, size_t *);
int bx_rsync_meta_recv(struct sess *, int, struct flist *);
int bx_rsync_meta_remap(struct sess *, const struct ident *, size_t,
    const struct ident *, size_t);
void bx_rsync_meta_cleanup(struct sess *);
/* Return an owned descriptor for the destination inode. */
int bx_rsync_meta_pin(int, const struct flist *, const char *);
int bx_rsync_meta_apply(struct sess *, int, int, const struct flist *,
    const char *, int);
int bx_rsync_meta_compare(struct sess *, int, const struct flist *,
    const char *, int32_t *);

#endif
