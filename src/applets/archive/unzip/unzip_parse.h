#ifndef BX_APPLETS_ARCHIVE_UNZIP_UNZIP_PARSE_H
#define BX_APPLETS_ARCHIVE_UNZIP_UNZIP_PARSE_H

#include <stdio.h>

#include "ctx.h"

int zu_unzip_parse_args(int argc, char** argv, ZContext* ctx);
void zu_unzip_print_version(FILE* to);
void zu_unzip_trace_effective_defaults(ZContext* ctx);

#endif /* BX_APPLETS_ARCHIVE_UNZIP_UNZIP_PARSE_H */
