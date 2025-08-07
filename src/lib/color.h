#ifndef BX_LIB_COLOR_H
#define BX_LIB_COLOR_H

#include <stdbool.h>

enum bx_color_mode {
    BX_COLOR_NEVER,
    BX_COLOR_AUTO,
    BX_COLOR_ALWAYS,
};

enum bx_color_mode bx_color_parse(const char *s);
const char *bx_color_red(void);
const char *bx_color_green(void);
const char *bx_color_blue(void);
const char *bx_color_cyan(void);
const char *bx_color_magenta(void);
const char *bx_color_yellow(void);
const char *bx_color_bold(void);
const char *bx_color_dim(void);
const char *bx_color_reset(void);

void bx_color_set_mode(enum bx_color_mode mode);
bool bx_color_enabled(void);

#endif
