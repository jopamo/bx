#ifndef BX_SEARCH_LITERAL_SCAN_H
#define BX_SEARCH_LITERAL_SCAN_H

#include <stddef.h>

#include "literal_plan.h"

struct bx_literal_matcher;

void bx_lit_plan_select_ops(struct bx_lit_plan *plan);
enum bx_lit_result bx_literal_scan_absent(const struct bx_lit_plan *plan,
                                          const unsigned char *buf,
                                          size_t len,
                                          size_t *match_off);
const struct bx_lit_plan *bx_literal_absence_plan(const struct bx_literal_matcher *m);

#endif
