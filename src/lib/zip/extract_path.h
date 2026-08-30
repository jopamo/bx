#ifndef ZU_EXTRACT_PATH_H
#define ZU_EXTRACT_PATH_H

#include <stdbool.h>

#include "ctx.h"

bool zu_extract_path_is_safe(const char* name);
int zu_extract_ensure_dir(const char* path);
int zu_extract_ensure_parent_dirs(const char* path);
char* zu_extract_build_output_path(const ZContext* ctx, const char* name);

#endif /* ZU_EXTRACT_PATH_H */
