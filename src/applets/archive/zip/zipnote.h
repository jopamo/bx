#ifndef BX_APPLETS_ARCHIVE_ZIP_ZIPNOTE_H
#define BX_APPLETS_ARCHIVE_ZIP_ZIPNOTE_H

#include "ctx.h"

int zu_zipnote_list(ZContext* ctx);
int zu_zipnote_apply(ZContext* ctx, const char* tool_name);

#endif /* BX_APPLETS_ARCHIVE_ZIP_ZIPNOTE_H */
