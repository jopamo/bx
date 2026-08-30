#ifndef BX_APPLETS_ARCHIVE_ZIP_ZIP_PARSE_H
#define BX_APPLETS_ARCHIVE_ZIP_ZIP_PARSE_H

#include <stdbool.h>
#include <stdio.h>

#include "ctx.h"

int zu_zip_parse_args(int argc, char** argv, ZContext* ctx, bool is_zipnote);
int zu_zip_read_comment(ZContext* ctx);
void zu_zip_print_version(FILE* to);

#endif /* BX_APPLETS_ARCHIVE_ZIP_ZIP_PARSE_H */
