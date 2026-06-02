#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "bx/libbx.h"
#include "fswalk/walk.h"
#include "lib/id_parse.h"
#include "lib/path_ops.h"
#include "lib/size_parse.h"
#include "tree_internal.h"

struct bx_tree_collect_state {
    const struct bx_tree_options *opts;
    struct bx_tree_root *root;
    struct bx_tree_node **stack;
    size_t stack_len;
    size_t stack_cap;
};

static bool bx_tree_stack_reserve(struct bx_tree_collect_state *state,
                                  size_t needed) {
    if (state->stack_cap >= needed)
        return true;

    size_t new_cap = state->stack_cap == 0 ? 16u : state->stack_cap * 2u;
    while (new_cap < needed)
        new_cap *= 2u;

    struct bx_tree_node **tmp = realloc(state->stack,
                                        new_cap * sizeof(*state->stack));
    if (!tmp)
        return false;

    state->stack = tmp;
    state->stack_cap = new_cap;
    return true;
}

static bool bx_tree_node_append_child(struct bx_tree_node *parent,
                                      struct bx_tree_node *child) {
    if (!parent || !child)
        return false;

    if (parent->child_count == parent->child_cap) {
        size_t new_cap = parent->child_cap == 0 ? 8u : parent->child_cap * 2u;
        struct bx_tree_node **tmp = realloc(parent->children,
                                            new_cap * sizeof(*parent->children));
        if (!tmp)
            return false;
        parent->children = tmp;
        parent->child_cap = new_cap;
    }

    parent->children[parent->child_count++] = child;
    child->parent = parent;
    return true;
}

static char *bx_tree_strdup_range(const char *start, size_t len) {
    char *s = malloc(len + 1u);
    if (!s)
        return NULL;
    memcpy(s, start, len);
    s[len] = '\0';
    return s;
}

static bool bx_tree_count_dir_entries(const char *path, size_t *count_out) {
    DIR *dir = opendir(path);
    if (!dir)
        return false;

    size_t count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        count++;
    }

    closedir(dir);
    *count_out = count;
    return true;
}

static bool bx_tree_match_segment(const char *pattern,
                                  const char *text,
                                  bool ignore_case) {
    if (!pattern || !text)
        return false;

    if (!ignore_case)
        return fnmatch(pattern, text, 0) == 0;

#ifdef FNM_CASEFOLD
    return fnmatch(pattern, text, FNM_CASEFOLD) == 0;
#else
    char *lower_pattern = xstrdup(pattern);
    char *lower_text = xstrdup(text);
    for (char *p = lower_pattern; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    for (char *p = lower_text; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    bool matched = fnmatch(lower_pattern, lower_text, 0) == 0;
    free(lower_pattern);
    free(lower_text);
    return matched;
#endif
}

static bool bx_tree_pattern_matches(const char *pattern,
                                    const char *text,
                                    bool ignore_case) {
    if (!pattern || !text)
        return false;

    const char *segment = pattern;
    while (true) {
        const char *pipe = strchr(segment, '|');
        size_t len = pipe ? (size_t)(pipe - segment) : strlen(segment);
        char *one = bx_tree_strdup_range(segment, len);
        if (!one)
            return false;
        bool matched = one[0] != '\0' && bx_tree_match_segment(one, text, ignore_case);
        free(one);
        if (matched)
            return true;
        if (!pipe)
            break;
        segment = pipe + 1;
    }

    return false;
}

static struct bx_tree_node *bx_tree_node_create(const char *label,
                                                const char *path,
                                                int depth,
                                                const struct bx_tree_options *opts) {
    struct bx_tree_node *node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;

    node->label = xstrdup(label);
    node->path = xstrdup(path);
    node->depth = depth;
    if (!node->label || !node->path) {
        bx_tree_free_root(&(struct bx_tree_root){.node = node});
        return NULL;
    }

    if (lstat(path, &node->lst) != 0) {
        bx_tree_free_root(&(struct bx_tree_root){.node = node});
        return NULL;
    }

    node->is_symlink = S_ISLNK(node->lst.st_mode);
    if (stat(path, &node->st) == 0)
        node->has_stat = true;
    else
        node->st = node->lst;

    node->follows_dir = false;
    if (node->is_symlink) {
        node->link_target = bx_path_readlink_dup(path);
        if (!node->link_target) {
            bx_tree_free_root(&(struct bx_tree_root){.node = node});
            return NULL;
        }
        if (opts->follow_symlink_dirs && node->has_stat && S_ISDIR(node->st.st_mode)) {
            node->is_dir = true;
            node->follows_dir = true;
        }
    }

    if (!node->is_dir)
        node->is_dir = S_ISDIR(node->lst.st_mode);

    node->include_match = bx_tree_pattern_matches(opts->pattern_include,
                                                  label,
                                                  opts->ignore_case);
    node->excluded_match = bx_tree_pattern_matches(opts->pattern_exclude,
                                                   label,
                                                   opts->ignore_case);
    return node;
}

static bool bx_tree_name_is_hidden(const char *label) {
    return label && label[0] == '.';
}

static enum bx_walk_action bx_tree_collect_callback(struct bx_walk_entry *entry, void *user) {
    struct bx_tree_collect_state *state = user;

    if (!state || !entry)
        return BX_WALK_CONTINUE;

    while (state->stack_len > (size_t)entry->depth)
        state->stack_len--;

    const char *label = NULL;
    if (entry->depth == 0) {
        label = state->root->operand;
    } else {
        label = bx_path_basename_ptr(entry->path);
    }

    if (entry->depth > 0 && !state->opts->show_all && bx_tree_name_is_hidden(label))
        return entry->is_dir ? BX_WALK_PRUNE : BX_WALK_CONTINUE;

    if (entry->is_dir && state->opts->filelimit >= 0) {
        size_t count = 0;
        if (bx_tree_count_dir_entries(entry->path, &count) &&
            count > (size_t)state->opts->filelimit) {
            entry->prune = true;
        }
    }

    struct bx_tree_node *node = bx_tree_node_create(label, entry->path,
                                                    entry->depth, state->opts);
    if (!node)
        return BX_WALK_ERROR;

    if (entry->is_dir && state->opts->filelimit >= 0) {
        size_t count = 0;
        if (bx_tree_count_dir_entries(entry->path, &count) &&
            count > (size_t)state->opts->filelimit) {
            node->filelimit_exceeded = true;
            node->filelimit_count = count;
        }
    }

    if (entry->depth == 0) {
        state->root->node = node;
    } else if (state->stack_len > 0) {
        if (!bx_tree_node_append_child(state->stack[state->stack_len - 1u], node)) {
            bx_tree_free_root(&(struct bx_tree_root){.node = node});
            return BX_WALK_ERROR;
        }
    }

    if (node->is_dir) {
        if (!bx_tree_stack_reserve(state, (size_t)entry->depth + 1u))
            return BX_WALK_ERROR;
        state->stack[state->stack_len++] = node;
    }
    return entry->prune ? BX_WALK_PRUNE : BX_WALK_CONTINUE;
}

static bool bx_tree_build_with_walk(const char *operand,
                                    const struct bx_tree_options *opts,
                                    struct bx_tree_root *out,
                                    bool *walk_failed) {
    bool stop = false;
    struct bx_walk_opts wopts = {
        .sort_entries = false,
        .follow_symlinks = opts->follow_symlink_dirs,
        .follow_root_symlink = opts->follow_symlink_dirs,
        .stay_on_filesystem = opts->stay_on_filesystem,
        .stop = &stop,
        .suppress_eacces = false,
        .suppress_errors = false,
        .report_eacces = true,
        .os_error_style = false,
        .error_prefix = opts->progname,
        .max_depth = opts->max_depth,
        .cycle_mode = BX_WALK_CYCLE_DIR_REPEAT,
        .cycle_report = BX_WALK_CYCLE_WARN,
    };
    struct bx_walk_ops ops = {
        .visit = bx_tree_collect_callback,
        .error = NULL,
    };

    struct bx_tree_collect_state state = {
        .opts = opts,
        .root = out,
    };

    int rc = bx_walk(operand, &wopts, &ops, &state);
    free(state.stack);
    if (rc != 0 && walk_failed)
        *walk_failed = true;
    return out->node != NULL;
}

bool bx_tree_build_root(const char *operand,
                        const struct bx_tree_options *opts,
                        struct bx_tree_root *out,
                        bool *walk_failed) {
    memset(out, 0, sizeof(*out));
    out->operand = xstrdup(operand);
    if (!out->operand)
        return false;

    return bx_tree_build_with_walk(operand, opts, out, walk_failed);
}

static bool bx_tree_apply_visibility_node(struct bx_tree_node *node,
                                          const struct bx_tree_options *opts,
                                          bool parent_include_disabled,
                                          bool is_root) {
    if (!node)
        return false;

    if (!is_root && node->excluded_match) {
        node->visible = false;
        return false;
    }

    bool dir_match_anchor = opts->pattern_include != NULL &&
                            opts->match_dirs &&
                            node->is_dir &&
                            node->include_match;
    bool include_disabled = parent_include_disabled || dir_match_anchor;
    bool child_visible = false;

    for (size_t i = 0; i < node->child_count; i++) {
        if (bx_tree_apply_visibility_node(node->children[i], opts,
                                          include_disabled, false)) {
            child_visible = true;
        }
    }

    if (is_root) {
        node->visible = true;
        return true;
    }

    if (!node->is_dir) {
        bool visible = true;
        if (opts->dirs_only)
            visible = false;
        if (visible && opts->pattern_include && !include_disabled)
            visible = node->include_match;
        node->visible = visible;
        return visible;
    }

    if (node->filelimit_exceeded || dir_match_anchor) {
        node->visible = true;
        return true;
    }

    if (opts->prune) {
        node->visible = child_visible;
        return child_visible;
    }

    node->visible = true;
    return true;
}

void bx_tree_apply_visibility(struct bx_tree_root *root,
                              const struct bx_tree_options *opts) {
    if (!root || !root->node)
        return;

    (void)bx_tree_apply_visibility_node(root->node, opts, false, true);
}

static const struct stat *bx_tree_display_stat(const struct bx_tree_node *node) {
    if (!node)
        return NULL;
    if (node->has_stat)
        return &node->st;
    return &node->lst;
}

static const struct bx_tree_options *bx_tree_sort_opts = NULL;

static int bx_tree_compare_nodes(const void *left, const void *right, void *user) {
    const struct bx_tree_options *opts = user ? user : bx_tree_sort_opts;
    const struct bx_tree_node *a = *(const struct bx_tree_node * const *)left;
    const struct bx_tree_node *b = *(const struct bx_tree_node * const *)right;

    if (opts->dirs_first && a->is_dir != b->is_dir)
        return a->is_dir ? -1 : 1;

    int cmp = 0;
    if (opts->sort_mode == BX_TREE_SORT_MTIME) {
        const struct stat *ast = bx_tree_display_stat(a);
        const struct stat *bst = bx_tree_display_stat(b);
        if (ast->st_mtime > bst->st_mtime)
            cmp = -1;
        else if (ast->st_mtime < bst->st_mtime)
            cmp = 1;
        else
            cmp = strcmp(a->label, b->label);
    } else if (opts->sort_mode == BX_TREE_SORT_VERSION) {
        cmp = strverscmp(a->label, b->label);
    } else {
        cmp = strcmp(a->label, b->label);
    }

    if (opts->reverse_sort)
        cmp = -cmp;
    return cmp;
}

static void bx_tree_sort_node(struct bx_tree_node *node,
                              const struct bx_tree_options *opts) {
    if (!node)
        return;

    if (node->child_count > 1) {
        bx_tree_sort_opts = opts;
        qsort_r(node->children, node->child_count, sizeof(node->children[0]),
                bx_tree_compare_nodes, NULL);
    }

    for (size_t i = 0; i < node->child_count; i++)
        bx_tree_sort_node(node->children[i], opts);
}

void bx_tree_sort_visible(struct bx_tree_root *root,
                          const struct bx_tree_options *opts) {
    if (!root || !root->node)
        return;
    bx_tree_sort_node(root->node, opts);
}

static size_t bx_tree_uint_width(uintmax_t value) {
    size_t width = 1;
    while (value >= 10u) {
        value /= 10u;
        width++;
    }
    return width;
}

static void bx_tree_format_size_value(off_t size,
                                      bool human,
                                      char buffer[32]) {
    if (!human) {
        snprintf(buffer, 32, "%jd", (intmax_t)size);
        return;
    }

    if (size < 0) {
        snprintf(buffer, 32, "%jdB", (intmax_t)size);
        return;
    }

    uintmax_t base = 0;
    if (!bx_size_unit_label_base_uintmax(BX_SIZE_UNIT_LABEL_IEC_PREFIX, &base)) {
        snprintf(buffer, 32, "%jdB", (intmax_t)size);
        return;
    }
    bx_size_format_human_round((uintmax_t)size, base, "BKMGTPE", true, buffer, 32);
}

static void bx_tree_collect_meta_widths_node(const struct bx_tree_node *node,
                                             const struct bx_tree_options *opts,
                                             struct bx_tree_meta_widths *widths) {
    if (!node || !node->visible)
        return;

    if (node->parent != NULL) {
        const struct stat *st = bx_tree_display_stat(node);
        if (!opts->dirs_only || node->is_dir) {
            if (opts->show_inode) {
                size_t width = bx_tree_uint_width((uintmax_t)st->st_ino);
                if (width > widths->inode)
                    widths->inode = width;
            }
            if (opts->show_device) {
                size_t width = bx_tree_uint_width((uintmax_t)st->st_dev);
                if (width > widths->device)
                    widths->device = width;
            }
            if (opts->show_user) {
                char buffer[32];
                const char *name = bx_id_user_name(st->st_uid, buffer);
                size_t width = strlen(name);
                if (width > widths->user)
                    widths->user = width;
            }
            if (opts->show_group) {
                char buffer[32];
                const char *name = bx_id_group_name(st->st_gid, buffer);
                size_t width = strlen(name);
                if (width > widths->group)
                    widths->group = width;
            }
            if (opts->show_size || opts->human_size) {
                char buffer[32];
                bx_tree_format_size_value(st->st_size, opts->human_size, buffer);
                size_t width = strlen(buffer);
                if (width > widths->size)
                    widths->size = width;
            }
        }
    }

    for (size_t i = 0; i < node->child_count; i++)
        bx_tree_collect_meta_widths_node(node->children[i], opts, widths);
}

void bx_tree_collect_meta_widths(const struct bx_tree_root *root,
                                 const struct bx_tree_options *opts,
                                 struct bx_tree_meta_widths *widths) {
    if (!widths)
        return;
    memset(widths, 0, sizeof(*widths));
    if (!root || !root->node)
        return;
    bx_tree_collect_meta_widths_node(root->node, opts, widths);
}

static void bx_tree_count_visible_node(const struct bx_tree_node *node,
                                       const struct bx_tree_options *opts,
                                       unsigned long *directories,
                                       unsigned long *files,
                                       bool is_root) {
    if (!node || !node->visible)
        return;

    if (!is_root) {
        if (node->is_dir) {
            (*directories)++;
        } else if (!opts->dirs_only) {
            (*files)++;
        }
    }

    for (size_t i = 0; i < node->child_count; i++)
        bx_tree_count_visible_node(node->children[i], opts, directories, files, false);
}

void bx_tree_count_visible(const struct bx_tree_root *root,
                           const struct bx_tree_options *opts,
                           unsigned long *directories,
                           unsigned long *files) {
    if (!directories || !files)
        return;
    if (!root || !root->node)
        return;
    bx_tree_count_visible_node(root->node, opts, directories, files, true);
}

static void bx_tree_free_node(struct bx_tree_node *node) {
    if (!node)
        return;
    for (size_t i = 0; i < node->child_count; i++)
        bx_tree_free_node(node->children[i]);
    free(node->children);
    free(node->label);
    free(node->path);
    free(node->link_target);
    free(node);
}

void bx_tree_free_root(struct bx_tree_root *root) {
    if (!root)
        return;
    free(root->operand);
    bx_tree_free_node(root->node);
    root->operand = NULL;
    root->node = NULL;
}
