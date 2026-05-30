#define _GNU_SOURCE
#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "fd_output.h"
#include "lib/file_info_fmt.h"
#include "lib/id_parse.h"
#include "lib/line_writer.h"
#include "lib/path_ops.h"
#include "lib/size_parse.h"

struct fd_detail_widths {
    size_t nlink;
    size_t user;
    size_t group;
    size_t size;
};

static bool fd_writer_vprintf(struct bx_line_writer *writer, const char *format,
                              va_list ap) {
    char stack[256];
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(stack, sizeof(stack), format, copy);
    va_end(copy);
    if (needed < 0) {
        if (errno == 0)
            errno = EIO;
        return false;
    }

    size_t len = (size_t)needed;
    if (len < sizeof(stack))
        return bx_line_writer_write(writer, stack, len);

    char *buffer = malloc(len + 1u);
    if (!buffer) {
        errno = ENOMEM;
        return false;
    }

    int written = vsnprintf(buffer, len + 1u, format, ap);
    if (written < 0 || (size_t)written != len) {
        int saved_errno = errno != 0 ? errno : EIO;
        free(buffer);
        errno = saved_errno;
        return false;
    }

    bool ok = bx_line_writer_write(writer, buffer, len);
    int saved_errno = errno;
    free(buffer);
    errno = saved_errno;
    return ok;
}

static bool fd_writer_printf(struct bx_line_writer *writer, const char *format,
                             ...) {
    va_list ap;
    va_start(ap, format);
    bool ok = fd_writer_vprintf(writer, format, ap);
    va_end(ap);
    return ok;
}

bool fd_print_match_output(struct bx_line_writer *writer,
                           const struct fd_render_ctx *ctx,
                           const struct fd_opts *opts,
                           const char *path, bool is_dir) {
    if (!opts->output_format) {
        return fd_print_path(writer, ctx, path, is_dir);
    }

    char *format_path = fd_render_format_path(ctx, path);
    if (!format_path)
        return false;

    char *expanded = fd_expand_placeholders(opts->output_format, format_path);
    free(format_path);
    if (!expanded)
        return false;

    bool ok = bx_line_writer_puts(writer, expanded) &&
              bx_line_writer_putc(writer, opts->print0 ? '\0' : '\n');
    free(expanded);
    return ok;
}

static bool fd_detail_items_reserve(struct fd_detail_items *items, int needed) {
    if (items->cap >= needed)
        return true;

    int new_cap = items->cap == 0 ? 16 : items->cap * 2;
    while (new_cap < needed)
        new_cap *= 2;

    struct fd_detail_item *tmp = realloc(items->v, (size_t)new_cap * sizeof(*items->v));
    if (!tmp)
        return false;
    items->v = tmp;
    items->cap = new_cap;
    return true;
}

static off_t fd_detail_display_size(const struct bx_walk_entry *entry) {
    if (!entry)
        return 0;

    if (entry->is_dir && entry->block_size > 0 && entry->size < (off_t)entry->block_size)
        return (off_t)entry->block_size;

    return entry->size;
}

bool fd_detail_items_append(struct fd_detail_items *items,
                            const struct fd_render_ctx *ctx,
                            struct bx_walk_entry *entry) {
    items->render_opts = ctx->opts;

    if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_OUTPUT))
        return false;
    if (!fd_detail_items_reserve(items, items->count + 1))
        return false;

    char *display_path = fd_render_exec_path(ctx, entry->path);
    if (!display_path)
        return false;

    char *symlink_target = NULL;
    if (S_ISLNK(entry->mode)) {
        symlink_target = bx_path_readlink_dup(entry->path);
        if (!symlink_target) {
            free(display_path);
            return false;
        }
    }

    items->v[items->count++] = (struct fd_detail_item){
        .display_path = display_path,
        .symlink_target = symlink_target,
        .mode = entry->mode,
        .nlink = entry->nlink,
        .uid = entry->uid,
        .gid = entry->gid,
        .size = fd_detail_display_size(entry),
        .mtime_sec = entry->mtime.tv_sec,
    };
    return true;
}

static const char *fd_user_name(uid_t uid, char numeric_buffer[32]) {
    return bx_id_user_name(uid, numeric_buffer);
}

static const char *fd_group_name(gid_t gid, char numeric_buffer[32]) {
    return bx_id_group_name(gid, numeric_buffer);
}

static void fd_format_size(intmax_t size, char buffer[32]) {
    bool negative = size < 0;
    uintmax_t magnitude = (uintmax_t)size;
    if (negative)
        magnitude = (uintmax_t)(-(size + 1)) + 1;

    uintmax_t base = 0;
    if (!bx_size_unit_label_base_uintmax(BX_SIZE_UNIT_LABEL_IEC_PREFIX, &base)) {
        snprintf(buffer, 32, "%jd", size);
        return;
    }

    if (!negative) {
        bx_size_format_human_round(magnitude, base, "BKMGTPEZYRQ", false, buffer, 32);
        return;
    }

    char magnitude_text[32];
    bx_size_format_human_round(magnitude, base, "BKMGTPEZYRQ", false, magnitude_text, sizeof(magnitude_text));
    snprintf(buffer, 32, "-%s", magnitude_text);
}

static size_t fd_uintmax_width(uintmax_t value) {
    size_t width = 1;
    while (value >= 10) {
        value /= 10;
        width++;
    }
    return width;
}

static int fd_detail_item_compare(const void *left, const void *right) {
    const struct fd_detail_item *a = left;
    const struct fd_detail_item *b = right;
    return strcmp(a->display_path, b->display_path);
}

static void fd_detail_widths_init(struct fd_detail_widths *widths) {
    widths->nlink = 1;
    widths->user = 1;
    widths->group = 1;
    widths->size = 1;
}

static void fd_detail_widths_update(struct fd_detail_widths *widths,
                                    const struct fd_detail_item *item) {
    size_t nlink_width = fd_uintmax_width((uintmax_t)item->nlink);
    if (nlink_width > widths->nlink)
        widths->nlink = nlink_width;

    char user_numeric[32];
    const char *user_name = fd_user_name(item->uid, user_numeric);
    size_t user_width = strlen(user_name);
    if (user_width > widths->user)
        widths->user = user_width;

    char group_numeric[32];
    const char *group_name = fd_group_name(item->gid, group_numeric);
    size_t group_width = strlen(group_name);
    if (group_width > widths->group)
        widths->group = group_width;

    char size_text[32];
    fd_format_size((intmax_t)item->size, size_text);
    size_t size_width = strlen(size_text);
    if (size_width > widths->size)
        widths->size = size_width;
}

int fd_detail_items_print(struct fd_detail_items *items,
                          struct bx_line_writer *writer) {
    if (items->count == 0)
        return 0;

    qsort(items->v, (size_t)items->count, sizeof(items->v[0]), fd_detail_item_compare);

    struct fd_detail_widths widths;
    fd_detail_widths_init(&widths);
    for (int i = 0; i < items->count; i++)
        fd_detail_widths_update(&widths, &items->v[i]);

    for (int i = 0; i < items->count; i++) {
        const struct fd_detail_item *item = &items->v[i];
        char mode[11];
        char user_numeric[32];
        char group_numeric[32];
        char timestamp[32];
        char size[32];
        char *display_path = fd_quote_output_path_dup(items->render_opts, item->display_path);
        char *symlink_target = NULL;

        if (!display_path)
            return 1;
        if (item->symlink_target) {
            symlink_target = fd_quote_output_path_dup(items->render_opts, item->symlink_target);
            if (!symlink_target) {
                free(display_path);
                return 1;
            }
        }

        bx_file_mode_to_string(item->mode, mode);
        const char *user_name = fd_user_name(item->uid, user_numeric);
        const char *group_name = fd_group_name(item->gid, group_numeric);
        bx_file_format_ls_timestamp(item->mtime_sec, timestamp);
        fd_format_size((intmax_t)item->size, size);

        bool ok =
            fd_writer_printf(writer, "%s %*" PRIuMAX " %-*s %-*s %*s %s ",
                             mode,
                             (int)widths.nlink,
                             (uintmax_t)item->nlink,
                             (int)widths.user,
                             user_name,
                             (int)widths.group,
                             group_name,
                             (int)widths.size,
                             size,
                             timestamp) &&
            bx_line_writer_puts(writer, display_path);

        if (ok && symlink_target)
            ok = bx_line_writer_puts(writer, " -> ") &&
                 bx_line_writer_puts(writer, symlink_target);
        if (ok)
            ok = bx_line_writer_putc(writer, '\n');
        free(display_path);
        free(symlink_target);
        if (!ok)
            return 1;
    }

    return 0;
}

void fd_detail_items_free(struct fd_detail_items *items) {
    if (!items)
        return;
    for (int i = 0; i < items->count; i++) {
        free(items->v[i].display_path);
        free(items->v[i].symlink_target);
    }
    free(items->v);
    items->v = NULL;
    items->count = 0;
    items->cap = 0;
}
