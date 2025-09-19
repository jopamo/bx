#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fd_output.h"

struct fd_detail_widths {
    size_t nlink;
    size_t user;
    size_t group;
    size_t size;
};

bool fd_print_match_output(const struct fd_render_ctx *ctx, const struct fd_opts *opts,
                           const char *path, bool is_dir) {
    if (!opts->output_format) {
        fd_print_path(ctx, path, is_dir);
        return true;
    }

    char *format_path = fd_render_format_path(ctx, path);
    if (!format_path)
        return false;

    char *expanded = fd_expand_placeholders(opts->output_format, format_path);
    free(format_path);
    if (!expanded)
        return false;

    printf("%s%c", expanded, opts->print0 ? '\0' : '\n');
    free(expanded);
    return true;
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

static char *fd_readlink_target(const char *path) {
    size_t cap = 128;
    char *target = malloc(cap + 1);
    if (!target)
        return NULL;

    for (;;) {
        ssize_t nread = readlink(path, target, cap);
        if (nread < 0) {
            free(target);
            return NULL;
        }
        if ((size_t)nread < cap) {
            target[nread] = '\0';
            return target;
        }

        if (cap > (SIZE_MAX / 2) - 1) {
            free(target);
            errno = ENOMEM;
            return NULL;
        }
        cap *= 2;
        char *tmp = realloc(target, cap + 1);
        if (!tmp) {
            free(target);
            return NULL;
        }
        target = tmp;
    }
}

bool fd_detail_items_append(struct fd_detail_items *items,
                            const struct fd_render_ctx *ctx,
                            struct walk_entry *entry) {
    if (!walk_entry_load_metadata(entry))
        return false;
    if (!fd_detail_items_reserve(items, items->count + 1))
        return false;

    char *display_path = fd_render_exec_path(ctx, entry->path);
    if (!display_path)
        return false;

    char *symlink_target = NULL;
    if (S_ISLNK(entry->mode)) {
        symlink_target = fd_readlink_target(entry->path);
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
        .size = entry->size,
        .mtime_sec = entry->mtime.tv_sec,
    };
    return true;
}

static char fd_mode_type_char(mode_t mode) {
    if (S_ISREG(mode))
        return '-';
    if (S_ISDIR(mode))
        return 'd';
    if (S_ISLNK(mode))
        return 'l';
    if (S_ISCHR(mode))
        return 'c';
    if (S_ISBLK(mode))
        return 'b';
    if (S_ISFIFO(mode))
        return 'p';
#ifdef S_ISSOCK
    if (S_ISSOCK(mode))
        return 's';
#endif
    return '?';
}

static void fd_mode_to_string(mode_t mode, char out[11]) {
    out[0] = fd_mode_type_char(mode);
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';

    if (mode & S_ISUID)
        out[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID)
        out[6] = (mode & S_IXGRP) ? 's' : 'S';
#ifdef S_ISVTX
    if (mode & S_ISVTX)
        out[9] = (mode & S_IXOTH) ? 't' : 'T';
#endif

    out[10] = '\0';
}

static const char *fd_user_name(uid_t uid, char numeric_buffer[32]) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && pw->pw_name[0])
        return pw->pw_name;

    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

static const char *fd_group_name(gid_t gid, char numeric_buffer[32]) {
    struct group *gr = getgrgid(gid);
    if (gr && gr->gr_name && gr->gr_name[0])
        return gr->gr_name;

    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}

static void fd_format_timestamp(time_t timestamp, char buffer[32]) {
    time_t now = time(NULL);
    if (now == (time_t)-1)
        now = timestamp;

    struct tm tm_value;
    if (!localtime_r(&timestamp, &tm_value)) {
        snprintf(buffer, 32, "??? ?? ??:??");
        return;
    }

    double delta = difftime(now, timestamp);
    if (delta < 0.0)
        delta = -delta;

    const char *fmt = (delta > (365.0 / 2.0) * 24.0 * 60.0 * 60.0 ||
                       timestamp > now + 3600)
        ? "%b %e  %Y"
        : "%b %e %H:%M";
    if (strftime(buffer, 32, fmt, &tm_value) == 0)
        snprintf(buffer, 32, "??? ?? ??:??");
}

static void fd_format_size(intmax_t size, char buffer[32]) {
    static const char *units[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    const double base = 1024.0;
    const size_t max_unit = (sizeof(units) / sizeof(units[0])) - 1;

    bool negative = size < 0;
    uintmax_t magnitude = (uintmax_t)size;
    if (negative)
        magnitude = (uintmax_t)(-(size + 1)) + 1;

    double value = (double)magnitude;
    size_t unit = 0;
    while (value >= base && unit < max_unit) {
        value /= base;
        unit++;
    }

    if (unit == 0) {
        snprintf(buffer, 32, "%" PRIdMAX, size);
        return;
    }

    const char *sign = negative ? "-" : "";
    if (value < 10.0)
        snprintf(buffer, 32, "%s%.1f%s", sign, value, units[unit]);
    else
        snprintf(buffer, 32, "%s%.0f%s", sign, value, units[unit]);
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

int fd_detail_items_print(struct fd_detail_items *items) {
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

        fd_mode_to_string(item->mode, mode);
        const char *user_name = fd_user_name(item->uid, user_numeric);
        const char *group_name = fd_group_name(item->gid, group_numeric);
        fd_format_timestamp(item->mtime_sec, timestamp);
        fd_format_size((intmax_t)item->size, size);

        printf("%s %*" PRIuMAX " %-*s %-*s %*s %s %s",
               mode,
               (int)widths.nlink,
               (uintmax_t)item->nlink,
               (int)widths.user,
               user_name,
               (int)widths.group,
               group_name,
               (int)widths.size,
               size,
               timestamp,
               item->display_path);

        if (item->symlink_target)
            printf(" -> %s", item->symlink_target);
        putchar('\n');
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
