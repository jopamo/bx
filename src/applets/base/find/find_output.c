#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "find_output.h"
#include "fswalk/walk.h"
#include "lib/file_info_fmt.h"
#include "lib/id_parse.h"
#include "lib/line_writer.h"
#include "lib/path_ops.h"

struct find_output_sink {
    FILE *fp;
    struct bx_line_writer *writer;
};

static bool find_sink_write_bytes(struct find_output_sink *sink,
                                  const void *data, size_t len) {
    if (len == 0)
        return true;
    if (!sink || !data) {
        errno = EINVAL;
        return false;
    }
    if (sink->writer)
        return bx_line_writer_write(sink->writer, data, len);
    if (!sink->fp) {
        errno = EINVAL;
        return false;
    }

    errno = 0;
    if (fwrite(data, 1, len, sink->fp) == len)
        return true;
    if (errno == 0)
        errno = EIO;
    return false;
}

static bool find_sink_write_char(struct find_output_sink *sink, char ch) {
    if (!sink) {
        errno = EINVAL;
        return false;
    }
    if (sink->writer)
        return bx_line_writer_putc(sink->writer, ch);
    if (!sink->fp) {
        errno = EINVAL;
        return false;
    }

    errno = 0;
    if (fputc((unsigned char)ch, sink->fp) != EOF)
        return true;
    if (errno == 0)
        errno = EIO;
    return false;
}

static bool find_sink_write_string(struct find_output_sink *sink,
                                   const char *text) {
    if (!text) {
        errno = EINVAL;
        return false;
    }
    return find_sink_write_bytes(sink, text, strlen(text));
}

static bool find_sink_vprintf(struct find_output_sink *sink, const char *format,
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
        return find_sink_write_bytes(sink, stack, len);

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

    bool ok = find_sink_write_bytes(sink, buffer, len);
    int saved_errno = errno;
    free(buffer);
    errno = saved_errno;
    return ok;
}

static bool find_sink_printf(struct find_output_sink *sink, const char *format,
                             ...) {
    va_list ap;
    va_start(ap, format);
    bool ok = find_sink_vprintf(sink, format, ap);
    va_end(ap);
    return ok;
}

bool find_write_path_file(const char *progname, const char *filename,
                          const char *path, char terminator) {
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        fprintf(stderr, "%s: %s: %s\n", progname, filename, strerror(errno));
        return false;
    }
    struct find_output_sink sink = {.fp = fp};
    bool ok = find_sink_write_string(&sink, path) &&
              find_sink_write_char(&sink, terminator);
    if (!ok)
        fprintf(stderr, "%s: %s: %s\n", progname, filename,
                strerror(errno ? errno : EIO));
    fclose(fp);
    return ok;
}

bool find_write_path_writer(struct bx_line_writer *writer, const char *path,
                            char terminator) {
    struct find_output_sink sink = {.writer = writer};
    return find_sink_write_string(&sink, path) &&
           find_sink_write_char(&sink, terminator);
}

static bool find_write_printf_format_sink(struct find_output_sink *sink,
                                          const char *format,
                                          struct bx_walk_entry *entry) {
    if (!format || !entry)
        return false;

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '\\') {
            i++;
            if (format[i] == '\0')
                return find_sink_write_char(sink, '\\');
            switch (format[i]) {
            case '\\':
                if (!find_sink_write_char(sink, '\\'))
                    return false;
                break;
            case '0':
                if (!find_sink_write_char(sink, '\0'))
                    return false;
                break;
            case 'a':
                if (!find_sink_write_char(sink, '\a'))
                    return false;
                break;
            case 'b':
                if (!find_sink_write_char(sink, '\b'))
                    return false;
                break;
            case 'f':
                if (!find_sink_write_char(sink, '\f'))
                    return false;
                break;
            case 'n':
                if (!find_sink_write_char(sink, '\n'))
                    return false;
                break;
            case 'r':
                if (!find_sink_write_char(sink, '\r'))
                    return false;
                break;
            case 't':
                if (!find_sink_write_char(sink, '\t'))
                    return false;
                break;
            case 'v':
                if (!find_sink_write_char(sink, '\v'))
                    return false;
                break;
            case 'c':
                return true;
            default:
                if (!find_sink_write_char(sink, '\\') ||
                    !find_sink_write_char(sink, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (format[i] == '%') {
            i++;
            if (format[i] == '\0')
                return find_sink_write_char(sink, '%');
            switch (format[i]) {
            case '%':
                if (!find_sink_write_char(sink, '%'))
                    return false;
                break;
            case 'p':
                if (!find_sink_write_string(sink, entry->path))
                    return false;
                break;
            case 'l': {
                if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_OUTPUT))
                    return false;
                if (!S_ISLNK(entry->mode))
                    break;
                char *target = bx_path_readlink_dup(entry->path);
                if (!target)
                    return false;
                bool ok = find_sink_write_string(sink, target);
                free(target);
                if (!ok)
                    return false;
                break;
            }
            case 'm': {
                if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_OUTPUT))
                    return false;
                if (!find_sink_printf(sink, "%o", entry->mode & 07777u))
                    return false;
                break;
            }
            case 'h': {
                char *dir = bx_path_dirname_dup(entry->path);
                if (!dir)
                    return false;
                bool ok = find_sink_write_string(sink, dir);
                free(dir);
                if (!ok)
                    return false;
                break;
            }
            default:
                if (!find_sink_write_char(sink, '%') ||
                    !find_sink_write_char(sink, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (!find_sink_write_char(sink, format[i]))
            return false;
    }

    return true;
}

bool find_write_printf_format(FILE *fp, const char *format,
                              struct bx_walk_entry *entry) {
    struct find_output_sink sink = {.fp = fp};
    return find_write_printf_format_sink(&sink, format, entry);
}

bool find_write_printf_format_writer(struct bx_line_writer *writer,
                                     const char *format,
                                     struct bx_walk_entry *entry) {
    struct find_output_sink sink = {.writer = writer};
    return find_write_printf_format_sink(&sink, format, entry);
}

static const char *find_user_name(uid_t uid, char numeric_buffer[32]) {
    return bx_id_user_name(uid, numeric_buffer);
}

static const char *find_group_name(gid_t gid, char numeric_buffer[32]) {
    return bx_id_group_name(gid, numeric_buffer);
}

static bool find_write_ls_entry_sink(struct find_output_sink *sink,
                                     const struct bx_walk_entry *entry) {
    if (!sink || !entry) {
        errno = EINVAL;
        return false;
    }

    struct stat lst;
    struct stat st;
    bool have_lstat = lstat(entry->path, &lst) == 0;
    bool have_stat = entry->follow_metadata && stat(entry->path, &st) == 0;
    const struct stat *display = NULL;
    if (have_stat)
        display = &st;
    else if (have_lstat)
        display = &lst;
    else
        return false;

    char mode[11];
    char user_numeric[32];
    char group_numeric[32];
    char timestamp[32];
    bx_file_mode_to_string(display->st_mode, mode);
    const char *user_name = find_user_name(display->st_uid, user_numeric);
    const char *group_name = find_group_name(display->st_gid, group_numeric);
    bx_file_format_ls_timestamp(display->st_mtime, timestamp);

    uintmax_t blocks = 0;
    if (display->st_blocks > 0)
        blocks = (uintmax_t)display->st_blocks / 2u;

    if (!find_sink_printf(sink,
                          "%10" PRIuMAX " %6" PRIuMAX " %s %3" PRIuMAX
                          " %-8s %-8s ",
                          (uintmax_t)display->st_ino, blocks, mode,
                          (uintmax_t)display->st_nlink, user_name,
                          group_name)) {
        return false;
    }

    if (S_ISCHR(display->st_mode) || S_ISBLK(display->st_mode)) {
        if (!find_sink_printf(sink, "%3" PRIuMAX ", %3" PRIuMAX " %s ",
                              (uintmax_t)major(display->st_rdev),
                              (uintmax_t)minor(display->st_rdev),
                              timestamp) ||
            !find_sink_write_string(sink, entry->path)) {
            return false;
        }
    } else {
        if (!find_sink_printf(sink, "%8jd %s ",
                              (intmax_t)display->st_size, timestamp) ||
            !find_sink_write_string(sink, entry->path)) {
            return false;
        }
    }

    if (have_lstat && S_ISLNK(lst.st_mode) && (!entry->follow_metadata || !have_stat)) {
        char *link_target = bx_path_readlink_dup(entry->path);
        if (link_target) {
            bool ok = find_sink_write_string(sink, " -> ") &&
                      find_sink_write_string(sink, link_target);
            free(link_target);
            if (!ok)
                return false;
        }
    }

    return find_sink_write_char(sink, '\n');
}

bool find_write_ls_entry(FILE *fp, const struct bx_walk_entry *entry) {
    struct find_output_sink sink = {.fp = fp};
    return find_write_ls_entry_sink(&sink, entry);
}

bool find_write_ls_entry_writer(struct bx_line_writer *writer,
                                const struct bx_walk_entry *entry) {
    struct find_output_sink sink = {.writer = writer};
    return find_write_ls_entry_sink(&sink, entry);
}
