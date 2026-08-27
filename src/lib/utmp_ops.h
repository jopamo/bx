#ifndef BX_LIB_UTMP_OPS_H
#define BX_LIB_UTMP_OPS_H

#include <sys/types.h>

/* Mark an existing process record dead. Missing records are not synthesized. */
void bx_utmp_mark_dead(pid_t pid);

#endif
