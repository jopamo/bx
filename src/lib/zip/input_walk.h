#ifndef ZU_INPUT_WALK_H
#define ZU_INPUT_WALK_H

#include <stdbool.h>

#include "ctx.h"

bool zu_should_include(const ZContext* ctx, const char* name);
int zu_expand_args(ZContext* ctx);

#endif /* ZU_INPUT_WALK_H */
