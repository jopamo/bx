#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fd_exec_render.h"
#include "lib/path_ops.h"

const char *fd_basename(const char *path) {
    return bx_path_basename_ptr(path);
}

static bool fd_match_placeholder_raw(const char *text, enum fd_placeholder_kind *kind, size_t *len) {
    if (strncmp(text, "{//}", 4) == 0) {
        *kind = FD_PH_DIRNAME;
        *len = 4;
        return true;
    }
    if (strncmp(text, "{/.}", 4) == 0) {
        *kind = FD_PH_BASENAME_STEM;
        *len = 4;
        return true;
    }
    if (strncmp(text, "{/}", 3) == 0) {
        *kind = FD_PH_BASENAME;
        *len = 3;
        return true;
    }
    if (strncmp(text, "{.}", 3) == 0) {
        *kind = FD_PH_PATH_STEM;
        *len = 3;
        return true;
    }
    if (strncmp(text, FD_PLACEHOLDER, sizeof(FD_PLACEHOLDER) - 1) == 0) {
        *kind = FD_PH_PATH;
        *len = sizeof(FD_PLACEHOLDER) - 1;
        return true;
    }
    return false;
}

static bool fd_match_placeholder_at(const char *arg, const char *text,
                                    enum fd_placeholder_kind *kind, size_t *len) {
    if (!fd_match_placeholder_raw(text, kind, len))
        return false;

    if (text > arg && text[-1] == '{')
        return false;
    if (text[*len] == '}')
        return false;
    return true;
}

size_t fd_placeholder_count(const char *arg) {
    size_t count = 0;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            (void)kind;
            count++;
            p += len;
            continue;
        }
        p++;
    }
    return count;
}

static const char *fd_stem_input_path(const char *path) {
    return bx_path_strip_dot_slash_prefix_ptr(path);
}

static char *fd_remove_last_extension(const char *path) {
    return bx_path_remove_last_extension_dup(path);
}

static char *fd_placeholder_value(enum fd_placeholder_kind kind, const char *path) {
    switch (kind) {
    case FD_PH_PATH:
        return strdup(path);
    case FD_PH_BASENAME:
        return strdup(fd_basename(path));
    case FD_PH_DIRNAME:
        return bx_path_dirname_dup(path);
    case FD_PH_PATH_STEM:
        return fd_remove_last_extension(fd_stem_input_path(path));
    case FD_PH_BASENAME_STEM:
        return fd_remove_last_extension(fd_basename(fd_stem_input_path(path)));
    case FD_PH_NONE:
        break;
    }
    return NULL;
}

char *fd_expand_placeholders(const char *arg, const char *path) {
    size_t out_len = 1;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            out_len++;
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            out_len++;
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            char *value = fd_placeholder_value(kind, path);
            if (!value)
                return NULL;
            out_len += strlen(value);
            free(value);
            p += len;
            continue;
        }
        out_len++;
        p++;
    }

    char *out = malloc(out_len);
    if (!out)
        return NULL;

    char *dst = out;
    for (const char *p = arg; *p; ) {
        enum fd_placeholder_kind kind = FD_PH_NONE;
        size_t len = 0;
        if (p[0] == '{' && p[1] == '{') {
            *dst++ = '{';
            p += 2;
            continue;
        }
        if (p[0] == '}' && p[1] == '}') {
            *dst++ = '}';
            p += 2;
            continue;
        }
        if (fd_match_placeholder_at(arg, p, &kind, &len)) {
            char *value = fd_placeholder_value(kind, path);
            size_t value_len;
            if (!value) {
                free(out);
                return NULL;
            }
            value_len = strlen(value);
            memcpy(dst, value, value_len);
            dst += value_len;
            free(value);
            p += len;
            continue;
        }
        *dst++ = *p++;
    }
    *dst = '\0';
    return out;
}

void fd_render_ctx_init(struct fd_render_ctx *ctx, const struct fd_opts *opts,
                        bool strip_implicit_dot_prefix, const char *cwd) {
    if (!ctx)
        return;
    ctx->opts = opts;
    ctx->strip_implicit_dot_prefix = strip_implicit_dot_prefix;
    ctx->cwd = cwd;
}

static char *fd_exec_path(const struct fd_render_ctx *ctx, const char *path) {
    const char *relative = path;

    if (!ctx->opts->absolute_path)
        return NULL;

    relative = bx_path_strip_dot_slash_prefix_ptr(relative);
    if (relative[0] == '/')
        return strdup(relative);

    if (!ctx->cwd || ctx->cwd[0] == '\0')
        return strdup(relative);
    return bx_path_join(ctx->cwd, relative);
}

static char *fd_apply_path_separator(const struct fd_opts *opts, const char *path) {
    const char *sep = opts->path_separator;
    if (!sep || strcmp(sep, "/") == 0)
        return strdup(path);

    size_t sep_len = strlen(sep);
    size_t slash_count = 0;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            slash_count++;
    }

    size_t path_len = strlen(path);
    size_t out_len = path_len + slash_count * sep_len;
    if (slash_count > 0)
        out_len -= slash_count;
    char *out = malloc(out_len + 1);
    if (!out)
        return NULL;

    char *dst = out;
    for (const char *p = path; *p; p++) {
        if (*p == '/') {
            memcpy(dst, sep, sep_len);
            dst += sep_len;
        } else {
            *dst++ = *p;
        }
    }
    *dst = '\0';
    return out;
}

static bool fd_should_strip_cwd_prefix(const struct fd_render_ctx *ctx, bool for_exec) {
    if (ctx->opts->absolute_path)
        return false;

    switch (ctx->opts->strip_cwd_prefix) {
    case FD_STRIP_CWD_PREFIX_ALWAYS:
    case FD_STRIP_CWD_PREFIX_AUTO:
        return ctx->strip_implicit_dot_prefix;
    case FD_STRIP_CWD_PREFIX_NEVER:
        return false;
    case FD_STRIP_CWD_PREFIX_UNSET:
        return !for_exec && ctx->strip_implicit_dot_prefix;
    }
    return false;
}

static char *fd_append_dir_suffix(const struct fd_opts *opts, char *path, bool is_dir) {
    if (!path || !is_dir)
        return path;

    const char *sep = opts->path_separator ? opts->path_separator : "/";
    size_t path_len = strlen(path);
    size_t sep_len = strlen(sep);
    char *out = realloc(path, path_len + sep_len + 1);
    if (!out)
        return path;
    memcpy(out + path_len, sep, sep_len);
    out[path_len + sep_len] = '\0';
    return out;
}

static char *fd_render_base_path(const struct fd_render_ctx *ctx, const char *path,
                                 bool for_exec) {
    char *base = NULL;
    if (ctx->opts->absolute_path) {
        base = fd_exec_path(ctx, path);
    } else {
        const char *relative = path;
        if (fd_should_strip_cwd_prefix(ctx, for_exec) &&
            relative != NULL)
            relative = bx_path_strip_dot_slash_prefix_ptr(relative);
        base = strdup(relative);
    }
    if (!base)
        return NULL;

    char *rendered = fd_apply_path_separator(ctx->opts, base);
    free(base);
    return rendered;
}

char *fd_render_output_path(const struct fd_render_ctx *ctx, const char *path, bool is_dir) {
    char *rendered = fd_render_base_path(ctx, path, false);
    return fd_append_dir_suffix(ctx->opts, rendered, is_dir);
}

char *fd_render_format_path(const struct fd_render_ctx *ctx, const char *path) {
    return fd_render_base_path(ctx, path, false);
}

char *fd_render_exec_path(const struct fd_render_ctx *ctx, const char *path) {
    return fd_render_base_path(ctx, path, true);
}

void fd_print_path(const struct fd_render_ctx *ctx, const char *path, bool is_dir) {
    char terminator = ctx->opts->print0 ? '\0' : '\n';
    char *rendered = fd_render_output_path(ctx, path, is_dir);
    if (!rendered)
        return;
    printf("%s%c", rendered, terminator);
    free(rendered);
}
