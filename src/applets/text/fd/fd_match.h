#ifndef BX_APPLETS_TEXT_FD_MATCH_H
#define BX_APPLETS_TEXT_FD_MATCH_H

#include <stdbool.h>

#include "fd_exec.h"
#include "fd_exec_render.h"
#include "fd_internal.h"
#include "fd_output.h"
#include "fswalk/walk.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

struct bx_line_writer;

struct fd_state {
    struct fd_opts *opts;
    pcre2_code *regexes[FD_MAX_AND_PATTERNS + 1];
    pcre2_match_data *match_data[FD_MAX_AND_PATTERNS + 1];
    int regex_count;
    bool *stop;
    bool strip_implicit_dot_prefix;
    char *cwd;
    struct fd_render_ctx render;
    struct fd_exec_items exec_items;
    bool exec_collect_failed;
    struct fd_detail_items detail_items;
    bool output_collect_failed;
    struct bx_line_writer *writer;
};

bool fd_state_init(struct fd_state *st, const char *progname,
                   struct fd_opts *opts, bool *stop,
                   bool using_implicit_root,
                   struct bx_line_writer *writer);
void fd_state_cleanup(struct fd_state *st);
enum bx_walk_action fd_walk_callback(struct bx_walk_entry *entry, void *user);

#endif
