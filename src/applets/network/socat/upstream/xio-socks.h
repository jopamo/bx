/* source: xio-socks.h */
/* Copyright Gerhard Rieger and contributors (see file CHANGES) */
/* Published under the GNU General Public License V.2, see file COPYING */

#ifndef __xio_socks_h_included
#define __xio_socks_h_included 1

struct socks4 {
   uint8_t  version;
   uint8_t  action;
   uint16_t port;
   uint32_t dest;
} ;
#define SIZEOF_STRUCT_SOCKS4 sizeof(struct socks4)

struct socks4u {
   uint8_t  version;
   uint8_t  action;
   uint16_t port;
   uint32_t dest;
   char userid[1];	/* just to have access via this struct */
} ;

extern const struct optdesc opt_socksport;
extern const struct optdesc opt_socksuser;
extern const struct optdesc opt_sockspass;

extern const struct addrdesc xioaddr_socks4_connect;
extern const struct addrdesc xioaddr_socks4a_connect;

extern int _xioopen_opt_socksport(struct opt *opts, char **socksport);
extern int _xioopen_socks4_init(const char *targetport, struct opt *opts, char **socksport, struct socks4u *sockhead, size_t *headlen);
extern int _xioopen_socks4_prepare(struct single *xfd, const char *hostname, int socks4a, struct socks4u *sockhead, ssize_t *headlen, int level);
extern int _xioopen_socks4_connect(struct single *xfd, struct socks4u *sockhead, size_t headlen, int level);

#endif /* !defined(__xio_socks_h_included) */
