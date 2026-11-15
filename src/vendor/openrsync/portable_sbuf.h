/* Minimal sbuf interface for Unix systems without FreeBSD's libutil. */
#ifndef BX_RSYNC_PORTABLE_SBUF_H
#define BX_RSYNC_PORTABLE_SBUF_H

#include <stddef.h>

struct sbuf;

struct sbuf *bx_rsync_sbuf_new_auto(void);
void bx_rsync_sbuf_delete(struct sbuf *);
int bx_rsync_sbuf_bcat(struct sbuf *, const void *, size_t);
int bx_rsync_sbuf_cat(struct sbuf *, const char *);
int bx_rsync_sbuf_putc(struct sbuf *, int);
int bx_rsync_sbuf_printf(struct sbuf *, const char *, ...);
int bx_rsync_sbuf_finish(struct sbuf *);
char *bx_rsync_sbuf_data(struct sbuf *);
size_t bx_rsync_sbuf_len(const struct sbuf *);

#define sbuf_new_auto bx_rsync_sbuf_new_auto
#define sbuf_delete bx_rsync_sbuf_delete
#define sbuf_bcat bx_rsync_sbuf_bcat
#define sbuf_cat bx_rsync_sbuf_cat
#define sbuf_putc bx_rsync_sbuf_putc
#define sbuf_printf bx_rsync_sbuf_printf
#define sbuf_finish bx_rsync_sbuf_finish
#define sbuf_data bx_rsync_sbuf_data
#define sbuf_len bx_rsync_sbuf_len

#endif
