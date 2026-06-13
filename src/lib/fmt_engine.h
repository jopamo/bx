#ifndef BX_LIB_FMT_ENGINE_H
#define BX_LIB_FMT_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct bx_diag_ctx;
struct bx_line_writer;

#define BX_FMT_ENGINE_DEFAULT_WIDTH 75u
#define BX_FMT_ENGINE_MAX_WIDTH 2500u

struct bx_fmt_engine_options {
    size_t width;
    size_t goal;
    bool crown_margin;
    bool split_only;
    bool tagged_paragraph;
    bool uniform_spacing;
    const char *prefix;
    size_t prefix_lead_space;
    size_t prefix_full_length;
    size_t prefix_length;
};

size_t bx_fmt_engine_default_goal_width(size_t width);
bool bx_fmt_engine_process_stream(FILE *stream,
                                  const struct bx_fmt_engine_options *options,
                                  struct bx_line_writer *writer,
                                  struct bx_diag_ctx *diag);

#endif /* BX_LIB_FMT_ENGINE_H */
