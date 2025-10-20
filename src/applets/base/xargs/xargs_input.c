#define _GNU_SOURCE
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xargs_input.h"

struct xargs_collect_ctx {
    struct xargs_items *items;
};

static bool xargs_items_append(struct xargs_items *items, const char *text,
                               int line_group) {
    if (items->count >= items->cap) {
        int new_cap = items->cap == 0 ? 16 : items->cap * 2;
        char **tmp = realloc(items->v, (size_t)new_cap * sizeof(*items->v));
        if (!tmp)
            return false;
        items->v = tmp;
        int *line_tmp =
            realloc(items->line_groups,
                    (size_t)new_cap * sizeof(*items->line_groups));
        if (!line_tmp)
            return false;
        items->line_groups = line_tmp;
        items->cap = new_cap;
    }
    items->v[items->count] = strdup(text);
    if (!items->v[items->count])
        return false;
    items->line_groups[items->count] = line_group;
    items->count++;
    return true;
}

static bool xargs_collect_sink(const char *text, int line_group, void *user) {
    struct xargs_collect_ctx *ctx = user;
    return xargs_items_append(ctx->items, text, line_group);
}

void xargs_items_free(struct xargs_items *items) {
    if (!items)
        return;
    for (int i = 0; i < items->count; i++)
        free(items->v[i]);
    free(items->v);
    free(items->line_groups);
    items->v = NULL;
    items->line_groups = NULL;
    items->count = 0;
    items->cap = 0;
}

static bool xargs_buf_append(char **buf, size_t *len, size_t *cap, int ch) {
    if (*len + 1 >= *cap) {
        size_t new_cap = *cap == 0 ? 64 : *cap * 2;
        char *tmp = realloc(*buf, new_cap);
        if (!tmp)
            return false;
        *buf = tmp;
        *cap = new_cap;
    }
    (*buf)[(*len)++] = (char)ch;
    (*buf)[*len] = '\0';
    return true;
}

static bool xargs_emit_item(const char *buf, bool have_item,
                            const char *logical_eof, bool *stop,
                            int line_group, xargs_item_sink_fn sink,
                            void *user) {
    if (!have_item)
        return true;
    if (logical_eof && strcmp(buf ? buf : "", logical_eof) == 0) {
        if (stop)
            *stop = true;
        return true;
    }
    return sink(buf ? buf : "", line_group, user);
}

static bool xargs_read_items_null(FILE *input, xargs_item_sink_fn sink,
                                  void *user) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int line_group = 1;
    int ch;

    while ((ch = fgetc(input)) != EOF) {
        if (ch == '\0') {
            if (!sink(buf ? buf : "", line_group++, user)) {
                free(buf);
                return false;
            }
            free(buf);
            buf = NULL;
            len = 0;
            cap = 0;
            continue;
        }
        if (!xargs_buf_append(&buf, &len, &cap, ch)) {
            free(buf);
            return false;
        }
    }

    if (buf && len > 0) {
        if (!sink(buf, line_group, user)) {
            free(buf);
            return false;
        }
    }
    free(buf);
    return true;
}

static bool xargs_read_items_delim(FILE *input, char delimiter,
                                   const char *logical_eof,
                                   xargs_item_sink_fn sink, void *user) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    bool have_item = false;
    bool stop = false;
    int line_group = 1;
    int ch;

    while (!stop && (ch = fgetc(input)) != EOF) {
        if ((char)ch == delimiter) {
            if (!xargs_emit_item(buf, true, logical_eof, &stop, line_group++,
                                 sink, user)) {
                free(buf);
                return false;
            }
            len = 0;
            if (buf)
                buf[0] = '\0';
            have_item = false;
            continue;
        }

        if (!xargs_buf_append(&buf, &len, &cap, ch)) {
            free(buf);
            return false;
        }
        have_item = true;
    }

    if (!stop && have_item) {
        if (!xargs_emit_item(buf, true, logical_eof, &stop, line_group, sink,
                             user)) {
            free(buf);
            return false;
        }
    }

    free(buf);
    return true;
}

static bool xargs_read_items_default(FILE *input, const char *progname,
                                     const char *logical_eof,
                                     xargs_item_sink_fn sink, void *user) {
    char *buf = NULL;
    size_t len = 0, cap = 0;
    int quote = 0;
    bool escaped = false;
    bool have_item = false;
    bool stop = false;
    int next_line_group = 1;
    int current_line_group = 0;
    int ch;

    while (!stop && (ch = fgetc(input)) != EOF) {
        if (quote == '\'') {
            if (ch == '\'') {
                quote = 0;
                have_item = true;
                continue;
            }
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (quote == '"') {
            if (escaped) {
                if (!xargs_buf_append(&buf, &len, &cap, ch))
                    goto oom;
                escaped = false;
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                have_item = true;
                continue;
            }
            if (ch == '\\') {
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                escaped = true;
                have_item = true;
                continue;
            }
            if (ch == '"') {
                if (current_line_group == 0)
                    current_line_group = next_line_group;
                quote = 0;
                have_item = true;
                continue;
            }
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (escaped) {
            if (!xargs_buf_append(&buf, &len, &cap, ch))
                goto oom;
            escaped = false;
            if (current_line_group == 0)
                current_line_group = next_line_group;
            have_item = true;
            continue;
        }

        if (ch == '\\') {
            if (current_line_group == 0)
                current_line_group = next_line_group;
            escaped = true;
            have_item = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            if (current_line_group == 0)
                current_line_group = next_line_group;
            quote = ch;
            have_item = true;
            continue;
        }
        if (isspace((unsigned char)ch)) {
            if (have_item) {
                if (!xargs_emit_item(buf, true, logical_eof, &stop,
                                     current_line_group ? current_line_group
                                                        : next_line_group,
                                     sink, user))
                    goto oom;
                len = 0;
                if (buf)
                    buf[0] = '\0';
                have_item = false;
            }
            if (ch == '\n') {
                if (current_line_group != 0)
                    next_line_group = current_line_group + 1;
                current_line_group = 0;
            }
            continue;
        }

        if (!xargs_buf_append(&buf, &len, &cap, ch))
            goto oom;
        if (current_line_group == 0)
            current_line_group = next_line_group;
        have_item = true;
    }

    if (quote || escaped) {
        fprintf(stderr, "%s: unterminated quote or escape in input\n",
                progname);
        free(buf);
        return false;
    }

    if (!stop && have_item) {
        if (!xargs_emit_item(buf, true, logical_eof, &stop,
                             current_line_group ? current_line_group
                                                : next_line_group,
                             sink, user))
            goto oom;
    }

    free(buf);
    return true;

oom:
    free(buf);
    return false;
}

static bool xargs_read_items_replace_lines(FILE *input,
                                           const char *logical_eof,
                                           xargs_item_sink_fn sink,
                                           void *user) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int line_group = 1;

    while ((len = getline(&line, &cap, input)) != -1) {
        while (len > 0 &&
               (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (logical_eof && strcmp(line, logical_eof) == 0)
            break;
        bool blank = true;
        for (ssize_t i = 0; i < len; i++) {
            if (!isspace((unsigned char)line[i])) {
                blank = false;
                break;
            }
        }
        if (blank)
            continue;
        if (!sink(line, line_group++, user)) {
            free(line);
            return false;
        }
    }

    free(line);
    return true;
}

bool xargs_read_stream(FILE *input, const char *progname,
                       struct xargs_opts *opts, xargs_item_sink_fn sink,
                       void *user) {
    if (!sink)
        return false;

    if (opts->replace_mode)
        return xargs_read_items_replace_lines(input, opts->logical_eof, sink,
                                              user);
    if (opts->nul_delim)
        return xargs_read_items_null(input, sink, user);
    if (opts->delimiter_mode)
        return xargs_read_items_delim(input, opts->delimiter, NULL, sink, user);
    return xargs_read_items_default(input, progname, opts->logical_eof, sink,
                                    user);
}

bool xargs_read_items(FILE *input, const char *progname, struct xargs_opts *opts,
                      struct xargs_items *items) {
    struct xargs_collect_ctx ctx = {.items = items};
    return xargs_read_stream(input, progname, opts, xargs_collect_sink, &ctx);
}
