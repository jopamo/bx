#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "color.h"

static enum bx_color_mode color_mode = BX_COLOR_AUTO;
static bool tty_checked = false;
static bool is_tty = false;

static bool check_tty(void) {
    if (!tty_checked) {
        is_tty = isatty(STDOUT_FILENO);
        tty_checked = true;
    }
    return is_tty;
}

enum bx_color_mode bx_color_parse(const char *s) {
    if (!s || strcmp(s, "never") == 0) return BX_COLOR_NEVER;
    if (strcmp(s, "always") == 0) return BX_COLOR_ALWAYS;
    if (strcmp(s, "auto") == 0) return BX_COLOR_AUTO;
    return BX_COLOR_AUTO;
}

void bx_color_set_mode(enum bx_color_mode mode) {
    color_mode = mode;
}

bool bx_color_enabled(void) {
    if (color_mode == BX_COLOR_ALWAYS) return true;
    if (color_mode == BX_COLOR_NEVER) return false;
    return check_tty();
}

#define SGR(code) (bx_color_enabled() ? "\033[" #code "m" : "")

const char *bx_color_red(void)     { return SGR(31); }
const char *bx_color_green(void)   { return SGR(32); }
const char *bx_color_blue(void)    { return SGR(34); }
const char *bx_color_cyan(void)    { return SGR(36); }
const char *bx_color_magenta(void) { return SGR(35); }
const char *bx_color_yellow(void)  { return SGR(33); }
const char *bx_color_bold(void)    { return SGR(1); }
const char *bx_color_dim(void)     { return SGR(2); }
const char *bx_color_reset(void)   { return SGR(0); }
