#ifndef BX_APPLETS_ARCHIVE_ARCHIVE_TEMP_H
#define BX_APPLETS_ARCHIVE_ARCHIVE_TEMP_H

#include <stdbool.h>

#include "lib/cancel_state.h"

bool bx_archive_temp_track(const char* path);
void bx_archive_temp_untrack(const char* path);
void bx_archive_temp_cleanup_all(void);
bool bx_archive_temp_install_signal_cleanup(void);
int bx_archive_temp_pending_signal(void);
void bx_archive_temp_clear_pending_signal(void);
struct bx_cancel_state* bx_archive_temp_cancel_state(void);

#endif /* BX_APPLETS_ARCHIVE_ARCHIVE_TEMP_H */
