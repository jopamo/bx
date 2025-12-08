#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bx/libbx.h"
#include "lib/file_info_fmt.h"
#include "lib/id_parse.h"
#include "tree_internal.h"

struct bx_tree_line_charset {
    const char *vert;
    const char *branch;
    const char *last;
    const char *space;
};

struct bx_tree_render_ctx {
    FILE *stream;
    const struct bx_tree_root *root;
    const struct bx_tree_options *opts;
    const struct bx_tree_meta_widths *widths;
    struct bx_diag_ctx *diag;
};

static char *bx_tree_xasprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *out = NULL;
    if (vasprintf(&out, fmt, ap) < 0) {
        va_end(ap);
        fprintf(stderr, "tree: out of memory\n");
        abort();
    }
    va_end(ap);
    return out;
}

static const struct stat *bx_tree_display_stat(const struct bx_tree_node *node) {
    if (!node)
        return NULL;
    if (node->has_stat)
        return &node->st;
    return &node->lst;
}

static const struct bx_tree_line_charset *bx_tree_charset(const struct bx_tree_options *opts) {
    static const struct bx_tree_line_charset utf8 = {
        .vert = "│   ",
        .branch = "├── ",
        .last = "└── ",
        .space = "    ",
    };
    static const struct bx_tree_line_charset ascii = {
        .vert = "|   ",
        .branch = "|-- ",
        .last = "`-- ",
        .space = "    ",
    };
    static const struct bx_tree_line_charset vt100 = {
        .vert = "\033(0x\033(B   ",
        .branch = "\033(0tqq\033(B ",
        .last = "\033(0mqq\033(B ",
        .space = "    ",
    };
    static const struct bx_tree_line_charset ibm437 = {
        .vert = "\263   ",
        .branch = "\303\304\304 ",
        .last = "\300\304\304 ",
        .space = "    ",
    };

    switch (opts->charset_mode) {
    case BX_TREE_CHARSET_ASCII:
        return &ascii;
    case BX_TREE_CHARSET_VT100:
        return &vt100;
    case BX_TREE_CHARSET_IBM437:
        return &ibm437;
    case BX_TREE_CHARSET_UTF8:
    default:
        return &utf8;
    }
}

static void bx_tree_format_size_value(off_t size,
                                      bool human,
                                      char buffer[32]) {
    if (!human) {
        snprintf(buffer, 32, "%jd", (intmax_t)size);
        return;
    }

    static const char *units[] = {"B", "K", "M", "G", "T", "P", "E"};
    double value = (double)size;
    size_t unit = 0u;
    while (value >= 1024.0 && unit + 1u < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        unit++;
    }

    if (unit == 0u)
        snprintf(buffer, 32, "%jdB", (intmax_t)size);
    else if (value >= 10.0)
        snprintf(buffer, 32, "%.0f%s", value, units[unit]);
    else
        snprintf(buffer, 32, "%.1f%s", value, units[unit]);
}

static bool bx_tree_lookup_ls_colors_key(const char *key,
                                         char *buffer,
                                         size_t buffer_size) {
    const char *ls_colors = getenv("LS_COLORS");
    if (!ls_colors || !*ls_colors || buffer_size == 0u)
        return false;

    size_t key_len = strlen(key);
    const char *cursor = ls_colors;
    const char *match_value = NULL;
    size_t match_len = 0u;

    while (*cursor) {
        const char *entry_start = cursor;
        while (*cursor && *cursor != ':')
            cursor++;
        const char *entry_end = cursor;
        const char *equal = memchr(entry_start, '=', (size_t)(entry_end - entry_start));
        if (equal && (size_t)(equal - entry_start) == key_len &&
            strncmp(entry_start, key, key_len) == 0) {
            match_value = equal + 1;
            match_len = (size_t)(entry_end - equal - 1);
        }
        if (*cursor == ':')
            cursor++;
    }

    if (!match_value)
        return false;

    if (match_len >= buffer_size)
        match_len = buffer_size - 1u;
    memcpy(buffer, match_value, match_len);
    buffer[match_len] = '\0';
    return true;
}

static bool bx_tree_lookup_ls_colors_suffix(const char *name,
                                            char *buffer,
                                            size_t buffer_size) {
    const char *ls_colors = getenv("LS_COLORS");
    if (!ls_colors || !*ls_colors || buffer_size == 0u)
        return false;

    const char *cursor = ls_colors;
    const char *match_value = NULL;
    size_t match_len = 0u;
    size_t name_len = strlen(name);

    while (*cursor) {
        const char *entry_start = cursor;
        while (*cursor && *cursor != ':')
            cursor++;
        const char *entry_end = cursor;
        const char *equal = memchr(entry_start, '=', (size_t)(entry_end - entry_start));
        if (equal && entry_start < equal && entry_start[0] == '*') {
            const char *suffix = entry_start + 1;
            size_t suffix_len = (size_t)(equal - suffix);
            if (suffix_len > 0u && suffix_len <= name_len &&
                memcmp(name + name_len - suffix_len, suffix, suffix_len) == 0) {
                match_value = equal + 1;
                match_len = (size_t)(entry_end - equal - 1);
            }
        }
        if (*cursor == ':')
            cursor++;
    }

    if (!match_value)
        return false;
    if (match_len >= buffer_size)
        match_len = buffer_size - 1u;
    memcpy(buffer, match_value, match_len);
    buffer[match_len] = '\0';
    return true;
}

static const char *bx_tree_default_color_code(const char *key) {
    if (strcmp(key, "di") == 0)
        return "01;34";
    if (strcmp(key, "ln") == 0)
        return "01;36";
    if (strcmp(key, "pi") == 0)
        return "33";
    if (strcmp(key, "so") == 0)
        return "01;35";
    if (strcmp(key, "bd") == 0 || strcmp(key, "cd") == 0)
        return "01;33";
    if (strcmp(key, "or") == 0)
        return "01;31";
    if (strcmp(key, "ex") == 0)
        return "01;32";
    if (strcmp(key, "rs") == 0)
        return "0";
    return "";
}

static const char *bx_tree_color_code_for_key(const char *key,
                                              char *buffer,
                                              size_t buffer_size) {
    if (bx_tree_lookup_ls_colors_key(key, buffer, buffer_size))
        return buffer;
    return bx_tree_default_color_code(key);
}

static const char *bx_tree_color_key_for_mode(mode_t mode) {
    if (S_ISDIR(mode))
        return "di";
    if (S_ISFIFO(mode))
        return "pi";
#ifdef S_ISSOCK
    if (S_ISSOCK(mode))
        return "so";
#endif
    if (S_ISBLK(mode))
        return "bd";
    if (S_ISCHR(mode))
        return "cd";
    return "no";
}

static const char *bx_tree_color_code_for_node(const struct bx_tree_node *node,
                                               char *buffer,
                                               size_t buffer_size) {
    mode_t mode = node->lst.st_mode;

    if (S_ISLNK(mode)) {
        if (!node->has_stat)
            return bx_tree_color_code_for_key("or", buffer, buffer_size);
        char link_buffer[64];
        if (bx_tree_lookup_ls_colors_key("ln", link_buffer, sizeof(link_buffer)) &&
            strcmp(link_buffer, "target") == 0) {
            mode_t target_mode = node->st.st_mode;
            if (S_ISREG(target_mode)) {
                if ((target_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
                    return bx_tree_color_code_for_key("ex", buffer, buffer_size);
                if (bx_tree_lookup_ls_colors_suffix(node->label, buffer, buffer_size))
                    return buffer;
                return bx_tree_color_code_for_key("fi", buffer, buffer_size);
            }
            return bx_tree_color_code_for_key(bx_tree_color_key_for_mode(target_mode),
                                              buffer, buffer_size);
        }
        return bx_tree_color_code_for_key("ln", buffer, buffer_size);
    }

    if (S_ISREG(mode)) {
        if ((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
            return bx_tree_color_code_for_key("ex", buffer, buffer_size);
        if (bx_tree_lookup_ls_colors_suffix(node->label, buffer, buffer_size))
            return buffer;
        return bx_tree_color_code_for_key("fi", buffer, buffer_size);
    }

    return bx_tree_color_code_for_key(bx_tree_color_key_for_mode(mode), buffer, buffer_size);
}

static char *bx_tree_wrap_ansi_color(const struct bx_tree_node *node,
                                     const struct bx_tree_options *opts,
                                     const char *text) {
    if (!opts->colorize)
        return xstrdup(text);

    char color_buffer[128];
    const char *color = bx_tree_color_code_for_node(node, color_buffer, sizeof(color_buffer));
    if (!color || !*color)
        return xstrdup(text);

    char reset_buffer[32];
    const char *reset = bx_tree_color_code_for_key("rs", reset_buffer, sizeof(reset_buffer));
    if (!reset || !*reset)
        reset = "0";

    return bx_tree_xasprintf("\033[%sm%s\033[%sm", color, text, reset);
}

static char *bx_tree_escape_name(const char *text,
                                 enum bx_tree_name_mode mode) {
    size_t cap = strlen(text) * 4u + 1u;
    char *out = xmalloc(cap);
    size_t pos = 0u;

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        unsigned char ch = *p;
        if (mode == BX_TREE_NAME_LITERAL) {
            out[pos++] = (char)ch;
            continue;
        }
        if (isprint(ch) || ch >= 0x80u) {
            out[pos++] = (char)ch;
            continue;
        }
        if (mode == BX_TREE_NAME_QUESTION) {
            out[pos++] = '?';
            continue;
        }
        out[pos++] = '^';
        out[pos++] = (ch == 127u) ? '?' : (char)(ch ^ 0x40u);
    }

    out[pos] = '\0';
    return out;
}

static char bx_tree_indicator(const struct bx_tree_node *node) {
    mode_t mode = bx_tree_display_stat(node)->st_mode;
    if (node->is_dir)
        return '/';
#ifdef S_ISSOCK
    if (S_ISSOCK(mode))
        return '=';
#endif
    if (S_ISFIFO(mode))
        return '|';
    if (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0)
        return '*';
    return '\0';
}

static bool bx_tree_write_string(FILE *stream,
                                 struct bx_diag_ctx *diag,
                                 const char *text) {
    if (fputs(text, stream) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tree_write_html_escaped(FILE *stream,
                                       struct bx_diag_ctx *diag,
                                       const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        switch (*p) {
        case '&':
            if (!bx_tree_write_string(stream, diag, "&amp;"))
                return false;
            break;
        case '<':
            if (!bx_tree_write_string(stream, diag, "&lt;"))
                return false;
            break;
        case '>':
            if (!bx_tree_write_string(stream, diag, "&gt;"))
                return false;
            break;
        case '"':
            if (!bx_tree_write_string(stream, diag, "&quot;"))
                return false;
            break;
        default:
            if (fputc(*p, stream) == EOF) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return false;
            }
            break;
        }
    }
    return true;
}

static char *bx_tree_url_encode(const char *text) {
    size_t len = 0u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-' || *p == '~')
            len += 1u;
        else
            len += 3u;
    }

    char *out = xmalloc(len + 1u);
    size_t pos = 0u;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (isalnum(*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-' || *p == '~') {
            out[pos++] = (char)*p;
        } else {
            snprintf(out + pos, 4u, "%%%02X", *p);
            pos += 3u;
        }
    }
    out[pos] = '\0';
    return out;
}

static char *bx_tree_node_href(const struct bx_tree_render_ctx *ctx,
                               const struct bx_tree_node *node,
                               const char *base_href) {
    if (!base_href)
        return NULL;

    if (node == ctx->root->node)
        return xstrdup(base_href);

    const char *root_path = ctx->root->node->path;
    const char *path = node->path;
    const char *relative = path;
    size_t root_len = strlen(root_path);
    if (strncmp(path, root_path, root_len) == 0) {
        relative = path + root_len;
        if (*relative == '/')
            relative++;
    }

    char *encoded = bx_tree_url_encode(relative);
    char *href = NULL;
    if (encoded[0] == '\0')
        href = xstrdup(base_href);
    else if (base_href[strlen(base_href) - 1u] == '/')
        href = bx_tree_xasprintf("%s%s", base_href, encoded);
    else
        href = bx_tree_xasprintf("%s/%s", base_href, encoded);
    free(encoded);
    return href;
}

static char *bx_tree_ansi_code_to_css(const char *code) {
    if (!code || !*code)
        return xstrdup("");

    const char *color = NULL;
    bool bold = false;
    bool underline = false;
    char *copy = xstrdup(code);
    char *saveptr = NULL;
    for (char *token = strtok_r(copy, ";", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ";", &saveptr)) {
        int value = atoi(token);
        switch (value) {
        case 1:
            bold = true;
            break;
        case 4:
            underline = true;
            break;
        case 30:
            color = "black";
            break;
        case 31:
            color = "red";
            break;
        case 32:
            color = "green";
            break;
        case 33:
            color = "olive";
            break;
        case 34:
            color = "blue";
            break;
        case 35:
            color = "magenta";
            break;
        case 36:
            color = "teal";
            break;
        case 37:
            color = "silver";
            break;
        case 90:
            color = "gray";
            break;
        case 91:
            color = "lightcoral";
            break;
        case 92:
            color = "lightgreen";
            break;
        case 93:
            color = "khaki";
            break;
        case 94:
            color = "lightskyblue";
            break;
        case 95:
            color = "violet";
            break;
        case 96:
            color = "paleturquoise";
            break;
        case 97:
            color = "white";
            break;
        default:
            break;
        }
    }
    free(copy);

    if (!color && !bold && !underline)
        return xstrdup("");

    char *style = xstrdup("");
    if (color) {
        char *tmp = bx_tree_xasprintf("%scolor:%s;", style, color);
        free(style);
        style = tmp;
    }
    if (bold) {
        char *tmp = bx_tree_xasprintf("%sfont-weight:bold;", style);
        free(style);
        style = tmp;
    }
    if (underline) {
        char *tmp = bx_tree_xasprintf("%stext-decoration:underline;", style);
        free(style);
        style = tmp;
    }
    return style;
}

static bool bx_tree_write_metadata(const struct bx_tree_render_ctx *ctx,
                                   const struct bx_tree_node *node) {
    const struct stat *st = bx_tree_display_stat(node);
    FILE *stream = ctx->stream;
    const struct bx_tree_meta_widths *widths = ctx->widths;

    if (ctx->opts->show_inode &&
        fprintf(stream, "%*ju ", (int)widths->inode, (uintmax_t)st->st_ino) < 0)
        goto write_error;
    if (ctx->opts->show_device &&
        fprintf(stream, "%*ju ", (int)widths->device, (uintmax_t)st->st_dev) < 0)
        goto write_error;
    if (ctx->opts->show_mode) {
        char mode[11];
        bx_file_mode_to_string(st->st_mode, mode);
        if (fprintf(stream, "[%s] ", mode) < 0)
            goto write_error;
    }
    if (ctx->opts->show_user) {
        char numeric[32];
        const char *name = bx_id_user_name(st->st_uid, numeric);
        if (fprintf(stream, "%-*s ", (int)widths->user, name) < 0)
            goto write_error;
    }
    if (ctx->opts->show_group) {
        char numeric[32];
        const char *name = bx_id_group_name(st->st_gid, numeric);
        if (fprintf(stream, "%-*s ", (int)widths->group, name) < 0)
            goto write_error;
    }
    if (ctx->opts->show_size || ctx->opts->human_size) {
        char size_buffer[32];
        bx_tree_format_size_value(st->st_size, ctx->opts->human_size, size_buffer);
        if (fprintf(stream, "%*s ", (int)widths->size, size_buffer) < 0)
            goto write_error;
    }
    if (ctx->opts->show_date) {
        char timestamp[32];
        bx_file_format_ls_timestamp(st->st_mtime, timestamp);
        if (fprintf(stream, "%s ", timestamp) < 0)
            goto write_error;
    }

    return true;

write_error:
    bx_diag(ctx->diag, "write error: %s", strerror(errno));
    return false;
}

static char *bx_tree_display_name_text(const struct bx_tree_render_ctx *ctx,
                                       const struct bx_tree_node *node) {
    const char *base = ctx->opts->full_path ? node->path : node->label;
    char *escaped = bx_tree_escape_name(base, ctx->opts->name_mode);
    if (ctx->opts->classify) {
        char indicator = bx_tree_indicator(node);
        if (indicator != '\0') {
            char *tmp = bx_tree_xasprintf("%s%c", escaped, indicator);
            free(escaped);
            escaped = tmp;
        }
    }
    return escaped;
}

static bool bx_tree_write_plain_line(const struct bx_tree_render_ctx *ctx,
                                     const struct bx_tree_node *node,
                                     const char *prefix) {
    char *display_name = bx_tree_display_name_text(ctx, node);
    char *colored = bx_tree_wrap_ansi_color(node, ctx->opts, display_name);

    bool ok = bx_tree_write_string(ctx->stream, ctx->diag, prefix);
    if (ok)
        ok = bx_tree_write_metadata(ctx, node);
    if (ok)
        ok = bx_tree_write_string(ctx->stream, ctx->diag, colored);
    if (ok && node->is_symlink)
        ok = fprintf(ctx->stream, " -> %s", node->link_target ? node->link_target : "") >= 0;
    if (ok && node->filelimit_exceeded)
        ok = fprintf(ctx->stream,
                     " [%zu entries exceeds filelimit, not opening dir]",
                     node->filelimit_count) >= 0;
    if (ok && fputc('\n', ctx->stream) == EOF)
        ok = false;

    if (!ok)
        bx_diag(ctx->diag, "write error: %s", strerror(errno));

    free(display_name);
    free(colored);
    return ok;
}

static bool bx_tree_render_plain_node(const struct bx_tree_render_ctx *ctx,
                                      const struct bx_tree_node *node,
                                      bool *ancestor_last,
                                      size_t ancestor_count,
                                      bool is_last,
                                      bool is_root) {
    if (!node || !node->visible)
        return true;

    const struct bx_tree_line_charset *lines = bx_tree_charset(ctx->opts);
    char *prefix = xstrdup("");
    if (!ctx->opts->no_indentation && !is_root) {
        for (size_t i = 0; i < ancestor_count; i++) {
            char *tmp = bx_tree_xasprintf("%s%s", prefix,
                                  ancestor_last[i] ? lines->space : lines->vert);
            free(prefix);
            prefix = tmp;
        }
        char *tmp = bx_tree_xasprintf("%s%s", prefix, is_last ? lines->last : lines->branch);
        free(prefix);
        prefix = tmp;
    }

    bool ok = bx_tree_write_plain_line(ctx, node, prefix);
    free(prefix);
    if (!ok)
        return false;

    size_t visible_children = 0u;
    for (size_t i = 0; i < node->child_count; i++)
        visible_children += node->children[i]->visible ? 1u : 0u;

    if (visible_children == 0u)
        return true;

    bool *next_ancestor_last = ancestor_last;
    size_t next_ancestor_count = ancestor_count;
    if (!ctx->opts->no_indentation && !is_root) {
        next_ancestor_last = xmalloc((ancestor_count + 1u) * sizeof(*next_ancestor_last));
        for (size_t i = 0; i < ancestor_count; i++)
            next_ancestor_last[i] = ancestor_last[i];
        next_ancestor_last[ancestor_count] = is_last;
        next_ancestor_count++;
    }

    size_t remaining = visible_children;
    for (size_t i = 0; i < node->child_count; i++) {
        const struct bx_tree_node *child = node->children[i];
        if (!child->visible)
            continue;
        remaining--;
        if (!bx_tree_render_plain_node(ctx, child,
                                       next_ancestor_last,
                                       next_ancestor_count,
                                       remaining == 0u,
                                       false)) {
            if (!ctx->opts->no_indentation && !is_root)
                free(next_ancestor_last);
            return false;
        }
    }

    if (!ctx->opts->no_indentation && !is_root)
        free(next_ancestor_last);
    return true;
}

bool bx_tree_render_plain(FILE *stream,
                          const struct bx_tree_root *root,
                          const struct bx_tree_options *opts,
                          const struct bx_tree_meta_widths *widths,
                          struct bx_diag_ctx *diag) {
    struct bx_tree_render_ctx ctx = {
        .stream = stream,
        .root = root,
        .opts = opts,
        .widths = widths,
        .diag = diag,
    };
    return bx_tree_render_plain_node(&ctx, root->node, NULL, 0u, true, true);
}

static bool bx_tree_write_html_name(const struct bx_tree_render_ctx *ctx,
                                    const struct bx_tree_node *node,
                                    const char *base_href) {
    char *display_name = bx_tree_display_name_text(ctx, node);
    bool ok = true;

    if (ctx->opts->colorize) {
        char color_code[128];
        const char *code = bx_tree_color_code_for_node(node, color_code, sizeof(color_code));
        char *style = bx_tree_ansi_code_to_css(code);
        if (style[0] != '\0') {
            if (fprintf(ctx->stream, "<span style=\"%s\">", style) < 0)
                ok = false;
        }
        free(style);
    }

    char *href = NULL;
    if (ok && !ctx->opts->html_no_links)
        href = bx_tree_node_href(ctx, node, base_href);
    if (ok && href) {
        if (fprintf(ctx->stream, "<a href=\"") < 0)
            ok = false;
        if (ok)
            ok = bx_tree_write_html_escaped(ctx->stream, ctx->diag, href);
        if (ok && fprintf(ctx->stream, "\">") < 0)
            ok = false;
    }

    if (ok)
        ok = bx_tree_write_html_escaped(ctx->stream, ctx->diag, display_name);
    if (ok && href && fprintf(ctx->stream, "</a>") < 0)
        ok = false;
    if (ok && ctx->opts->colorize) {
        char color_code[128];
        const char *code = bx_tree_color_code_for_node(node, color_code, sizeof(color_code));
        char *style = bx_tree_ansi_code_to_css(code);
        if (style[0] != '\0' && fprintf(ctx->stream, "</span>") < 0)
            ok = false;
        free(style);
    }

    free(href);
    free(display_name);
    if (!ok)
        bx_diag(ctx->diag, "write error: %s", strerror(errno));
    return ok;
}

static bool bx_tree_render_html_node(const struct bx_tree_render_ctx *ctx,
                                     const struct bx_tree_node *node,
                                     bool *ancestor_last,
                                     size_t ancestor_count,
                                     bool is_last,
                                     bool is_root,
                                     const char *base_href) {
    if (!node || !node->visible)
        return true;

    const struct bx_tree_line_charset *lines = bx_tree_charset(ctx->opts);
    if (!ctx->opts->no_indentation && !is_root) {
        for (size_t i = 0; i < ancestor_count; i++) {
            if (!bx_tree_write_html_escaped(ctx->stream, ctx->diag,
                                            ancestor_last[i] ? lines->space : lines->vert))
                return false;
        }
        if (!bx_tree_write_html_escaped(ctx->stream, ctx->diag,
                                        is_last ? lines->last : lines->branch))
            return false;
    }

    if (!bx_tree_write_metadata(ctx, node))
        return false;
    if (!bx_tree_write_html_name(ctx, node, base_href))
        return false;
    if (node->is_symlink) {
        if (!bx_tree_write_string(ctx->stream, ctx->diag, " -&gt; "))
            return false;
        if (!bx_tree_write_html_escaped(ctx->stream, ctx->diag,
                                        node->link_target ? node->link_target : ""))
            return false;
    }
    if (node->filelimit_exceeded) {
        if (fprintf(ctx->stream,
                    " [%zu entries exceeds filelimit, not opening dir]",
                    node->filelimit_count) < 0) {
            bx_diag(ctx->diag, "write error: %s", strerror(errno));
            return false;
        }
    }
    if (fputc('\n', ctx->stream) == EOF) {
        bx_diag(ctx->diag, "write error: %s", strerror(errno));
        return false;
    }

    size_t visible_children = 0u;
    for (size_t i = 0; i < node->child_count; i++)
        visible_children += node->children[i]->visible ? 1u : 0u;
    if (visible_children == 0u)
        return true;

    bool *next_ancestor_last = ancestor_last;
    size_t next_ancestor_count = ancestor_count;
    if (!ctx->opts->no_indentation && !is_root) {
        next_ancestor_last = xmalloc((ancestor_count + 1u) * sizeof(*next_ancestor_last));
        for (size_t i = 0; i < ancestor_count; i++)
            next_ancestor_last[i] = ancestor_last[i];
        next_ancestor_last[ancestor_count] = is_last;
        next_ancestor_count++;
    }

    size_t remaining = visible_children;
    for (size_t i = 0; i < node->child_count; i++) {
        const struct bx_tree_node *child = node->children[i];
        if (!child->visible)
            continue;
        remaining--;
        if (!bx_tree_render_html_node(ctx, child,
                                      next_ancestor_last,
                                      next_ancestor_count,
                                      remaining == 0u,
                                      false,
                                      base_href)) {
            if (!ctx->opts->no_indentation && !is_root)
                free(next_ancestor_last);
            return false;
        }
    }

    if (!ctx->opts->no_indentation && !is_root)
        free(next_ancestor_last);
    return true;
}

bool bx_tree_render_html(FILE *stream,
                         const struct bx_tree_root *root,
                         const struct bx_tree_options *opts,
                         const struct bx_tree_meta_widths *widths,
                         struct bx_diag_ctx *diag,
                         int depth_limit_override,
                         const char *output_title,
                         const char *base_href_override) {
    (void)depth_limit_override;
    struct bx_tree_render_ctx ctx = {
        .stream = stream,
        .root = root,
        .opts = opts,
        .widths = widths,
        .diag = diag,
    };
    const char *title = output_title ? output_title :
                        (opts->html_title ? opts->html_title : root->operand);
    const char *charset = opts->charset_name ? opts->charset_name : "UTF-8";
    const char *base_href = base_href_override ? base_href_override : opts->html_base_href;

    if (fprintf(stream,
                "<!DOCTYPE html>\n<html><head><meta charset=\"%s\"><title>",
                charset) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (!bx_tree_write_html_escaped(stream, diag, title))
        return false;
    if (fprintf(stream, "</title></head><body><h1>") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (!bx_tree_write_html_escaped(stream, diag, title))
        return false;
    if (fprintf(stream, "</h1><pre>\n") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (!bx_tree_render_html_node(&ctx, root->node, NULL, 0u, true, true, base_href))
        return false;
    if (fprintf(stream, "</pre></body></html>\n") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_tree_render_recursive_html_node(const struct bx_tree_options *opts,
                                               const struct bx_tree_meta_widths *widths,
                                               struct bx_diag_ctx *diag,
                                               struct bx_tree_node *node) {
    if (!node || !node->visible || !node->is_dir)
        return true;

    char *path = bx_tree_xasprintf("%s/00Tree.html", node->path);
    FILE *stream = fopen(path, "w");
    if (!stream) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        free(path);
        return false;
    }

    struct bx_tree_root subroot = {
        .operand = (char *)node->label,
        .node = (struct bx_tree_node *)node,
    };
    const char *title = node->label;
    bool ok = bx_tree_render_html(stream, &subroot, opts, widths, diag, -1, title,
                                  opts->html_base_href);
    if (fclose(stream) != 0)
        ok = false;
    free(path);
    if (!ok)
        return false;

    for (size_t i = 0; i < node->child_count; i++) {
        if (!bx_tree_render_recursive_html_node(opts, widths, diag,
                                                node->children[i]))
            return false;
    }
    return true;
}

bool bx_tree_render_recursive_html(const struct bx_tree_root *root,
                                   const struct bx_tree_options *opts,
                                   const struct bx_tree_meta_widths *widths,
                                   struct bx_diag_ctx *diag) {
    return bx_tree_render_recursive_html_node(opts, widths, diag, root->node);
}
