#ifndef BX_REGEX_LITERAL_H
#define BX_REGEX_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include "pcre2_matcher.h"

struct bx_literal_matcher;

int bx_literal_compile(struct bx_literal_matcher **out, const char *pattern, bool ignore_case);
int bx_literal_find(struct bx_literal_matcher *m, const unsigned char *buf, size_t len,
                    size_t start, struct bx_match *out);
bool bx_literal_contains_byte(const struct bx_literal_matcher *m, unsigned char byte);
void bx_literal_free(struct bx_literal_matcher *m);

#endif
