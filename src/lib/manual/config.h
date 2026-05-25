#ifndef BX_LIB_MANUAL_CONFIG_H
#define BX_LIB_MANUAL_CONFIG_H

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stddef.h>
#include <sys/types.h>

#define MAN_CONF_FILE "/etc/man.conf"
#define MANPATH_BASE "/usr/share/man:/usr/local/share/man"
#define MANPATH_DEFAULT "/usr/local/share/man:/usr/share/man"
#define OSENUM MANDOC_OS_OTHER
#define UTF8_LOCALE BX_MANUAL_UTF8_LOCALE

#define HAVE_DIRENT_NAMLEN 0
#define HAVE_BZLIB BX_HAVE_LIBBZ2
#define HAVE_ERR 1
#define HAVE_LESS_T 0
#define HAVE_OHASH 0
#define HAVE_PLEDGE 0
#define HAVE_PROGNAME 0
#define HAVE_SANDBOX_INIT 0
#define HAVE_STRPTIME 1
#define HAVE_WCHAR BX_MANUAL_HAVE_WCHAR

#define BINM_MAN "man"
#define BINM_PAGER "less"

void setprogname(const char *name);
const char *getprogname(void);
void *recallocarray(void *ptr, size_t oldnmemb, size_t newnmemb, size_t size);
size_t strlcat(char *dst, const char *src, size_t dsize);
size_t strlcpy(char *dst, const char *src, size_t dsize);
long long strtonum(const char *numstr, long long minval, long long maxval,
                   const char **errstrp);

#endif
