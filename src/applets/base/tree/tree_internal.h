#ifndef BX_APPLETS_BASE_TREE_INTERNAL_H
#define BX_APPLETS_BASE_TREE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

#include "bx/diag.h"

enum bx_tree_sort_mode {
    BX_TREE_SORT_NAME = 0,
    BX_TREE_SORT_MTIME,
    BX_TREE_SORT_VERSION,
};

enum bx_tree_name_mode {
    BX_TREE_NAME_CARET = 0,
    BX_TREE_NAME_QUESTION,
    BX_TREE_NAME_LITERAL,
};

enum bx_tree_charset_mode {
    BX_TREE_CHARSET_UTF8 = 0,
    BX_TREE_CHARSET_ASCII,
    BX_TREE_CHARSET_VT100,
    BX_TREE_CHARSET_IBM437,
};

struct bx_tree_options {
    const char *progname;
    bool show_all;
    bool dirs_only;
    bool full_path;
    bool no_indentation;
    bool follow_symlink_dirs;
    bool stay_on_filesystem;
    bool no_report;
    bool show_mode;
    bool show_size;
    bool human_size;
    bool show_user;
    bool show_group;
    bool show_date;
    bool show_inode;
    bool show_device;
    bool classify;
    bool match_dirs;
    bool ignore_case;
    bool prune;
    bool dirs_first;
    bool colorize;
    bool html_output;
    bool html_no_links;
    bool recursive_html;
    enum bx_tree_name_mode name_mode;
    enum bx_tree_sort_mode sort_mode;
    bool reverse_sort;
    enum bx_tree_charset_mode charset_mode;
    int max_depth;
    int filelimit;
    const char *pattern_include;
    const char *pattern_exclude;
    const char *html_base_href;
    const char *html_title;
    const char *output_path;
    const char *charset_name;
};

struct bx_tree_node {
    char *label;
    char *path;
    char *link_target;
    struct stat lst;
    struct stat st;
    bool has_stat;
    bool is_symlink;
    bool is_dir;
    bool follows_dir;
    bool filelimit_exceeded;
    size_t filelimit_count;
    bool include_match;
    bool excluded_match;
    bool visible;
    struct bx_tree_node *parent;
    struct bx_tree_node **children;
    size_t child_count;
    size_t child_cap;
    int depth;
};

struct bx_tree_root {
    char *operand;
    struct bx_tree_node *node;
};

struct bx_tree_meta_widths {
    size_t inode;
    size_t device;
    size_t user;
    size_t group;
    size_t size;
};

bool bx_tree_build_root(const char *operand,
                        const struct bx_tree_options *opts,
                        struct bx_tree_root *out,
                        bool *walk_failed);
void bx_tree_apply_visibility(struct bx_tree_root *root,
                              const struct bx_tree_options *opts);
void bx_tree_sort_visible(struct bx_tree_root *root,
                          const struct bx_tree_options *opts);
void bx_tree_collect_meta_widths(const struct bx_tree_root *root,
                                 const struct bx_tree_options *opts,
                                 struct bx_tree_meta_widths *widths);
void bx_tree_count_visible(const struct bx_tree_root *root,
                           const struct bx_tree_options *opts,
                           unsigned long *directories,
                           unsigned long *files);
void bx_tree_free_root(struct bx_tree_root *root);

bool bx_tree_render_plain(FILE *stream,
                          const struct bx_tree_root *root,
                          const struct bx_tree_options *opts,
                          const struct bx_tree_meta_widths *widths,
                          struct bx_diag_ctx *diag);
bool bx_tree_render_html(FILE *stream,
                         const struct bx_tree_root *root,
                         const struct bx_tree_options *opts,
                         const struct bx_tree_meta_widths *widths,
                         struct bx_diag_ctx *diag,
                         int depth_limit_override,
                         const char *output_title,
                         const char *base_href_override);
bool bx_tree_render_recursive_html(const struct bx_tree_root *root,
                                   const struct bx_tree_options *opts,
                                   const struct bx_tree_meta_widths *widths,
                                   struct bx_diag_ctx *diag);

#endif
