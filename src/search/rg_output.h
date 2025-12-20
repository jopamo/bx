#ifndef BX_SEARCH_RG_OUTPUT_H
#define BX_SEARCH_RG_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

struct bx_rg_basic_color {
    bool set;
    int code;
};

struct bx_rg_rgb_color {
    bool set;
    unsigned int red;
    unsigned int green;
    unsigned int blue;
};

struct bx_rg_ansi_color {
    bool set;
    unsigned int index;
};

struct bx_rg_color_style {
    bool none;
    struct bx_rg_basic_color fg_basic;
    struct bx_rg_basic_color bg_basic;
    struct bx_rg_ansi_color fg_ansi256;
    struct bx_rg_ansi_color bg_ansi256;
    struct bx_rg_rgb_color fg_rgb;
    struct bx_rg_rgb_color bg_rgb;
    bool bold;
    bool dim;
    bool underline;
};

struct bx_rg_color_settings {
    struct bx_rg_color_style path;
    struct bx_rg_color_style line;
    struct bx_rg_color_style column;
    struct bx_rg_color_style match;
};

void bx_rg_color_settings_init_defaults(struct bx_rg_color_settings *settings);
bool bx_rg_parse_colors_spec(const char *progname, const char *spec,
                             struct bx_rg_color_settings *settings);
void bx_rg_emit_color_style_start(const struct bx_rg_color_style *style);
void bx_rg_emit_color_reset(void);

char *bx_rg_display_path_dup(const char *path, bool strip_dot_prefix,
                             char path_separator);
bool bx_rg_parse_path_separator(const char *progname, const char *arg,
                                char *out_separator);

bool bx_rg_parse_hyperlink_format(const char *progname, const char *arg,
                                  char **out_format);
char *bx_rg_hyperlink_open_dup(const char *format, const char *hostname_bin,
                               const char *path, size_t line, size_t column,
                               bool have_line, bool have_column);
const char *bx_rg_hyperlink_close(void);

#endif
