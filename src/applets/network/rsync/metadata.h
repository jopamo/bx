#ifndef BX_RSYNC_METADATA_H
#define BX_RSYNC_METADATA_H

#include <stddef.h>
#include <stdint.h>

struct sess;
struct flist;
struct ident;

int bx_rsync_meta_negotiate(struct sess *, int, int);
int bx_rsync_meta_send(struct sess *, int, const struct flist *,
    struct ident **, size_t *, struct ident **, size_t *);
int bx_rsync_meta_recv(struct sess *, int, struct flist *);
int bx_rsync_meta_remap(struct sess *, const struct ident *, size_t,
    const struct ident *, size_t);
void bx_rsync_meta_cleanup(struct sess *);
int bx_rsync_meta_apply(struct sess *, int, int, const struct flist *,
    const char *, int);
int bx_rsync_meta_compare(struct sess *, int, const struct flist *,
    const char *, int32_t *);

#endif
