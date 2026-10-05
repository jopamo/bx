#ifndef BX_FETCH_STATE_DIRECTORY_H
#define BX_FETCH_STATE_DIRECTORY_H
/* BX_FETCH_HEADER_OWNER: store */
/* BX_FETCH_HEADER_CONSUMERS: store */

/* Allocates the shared persistent state directory name; does not open it. */
char* bx_fetch_state_directory(void);
#endif
