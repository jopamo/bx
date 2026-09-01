#ifndef BX_REGEX_PCRE2_MATCHER_H
#define BX_REGEX_PCRE2_MATCHER_H

#include <stdbool.h>
#include <stddef.h>

struct bx_regex;
struct bx_match {
    size_t start;
    size_t end;
};

enum bx_regex_flags {
    BX_REGEX_DEFAULT  = 0,
    BX_REGEX_ICASE    = 1 << 0,
    BX_REGEX_MULTILINE = 1 << 1,
    BX_REGEX_DOTALL   = 1 << 2,
};

int bx_regex_compile(struct bx_regex **out, const char *pattern, int flags, char **errmsg);
int bx_regex_find(struct bx_regex *rx, const unsigned char *buf, size_t len,
                  size_t start, struct bx_match *match);
int bx_regex_error(const struct bx_regex *rx);
void bx_regex_free(struct bx_regex *rx);
void bx_regex_print_version(void);

#endif
