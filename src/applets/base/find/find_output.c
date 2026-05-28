#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
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
#include "lib/path_ops.h"

static bool find_write_stream_bytes(FILE *fp, const void *data, size_t len) {
    return len == 0 || fwrite(data, 1, len, fp) == len;
}

static bool find_write_stream_char(FILE *fp, char ch) {
    return fputc((unsigned char)ch, fp) != EOF;
}

bool find_write_path_file(const char *progname, const char *filename,
                          const char *path, char terminator) {
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        fprintf(stderr, "%s: %s: %s\n", progname, filename, strerror(errno));
        return false;
    }
    size_t path_len = strlen(path);
    bool ok = fwrite(path, 1, path_len, fp) == path_len &&
              fputc(terminator, fp) != EOF;
    if (!ok)
        fprintf(stderr, "%s: %s: %s\n", progname, filename,
                strerror(errno ? errno : EIO));
    fclose(fp);
    return ok;
}

bool find_write_printf_format(FILE *fp, const char *format,
                              struct bx_walk_entry *entry) {
    if (!format || !entry)
        return false;

    for (size_t i = 0; format[i] != '\0'; i++) {
        if (format[i] == '\\') {
            i++;
            if (format[i] == '\0')
                return find_write_stream_char(fp, '\\');
            switch (format[i]) {
            case '\\':
                if (!find_write_stream_char(fp, '\\'))
                    return false;
                break;
            case '0':
                if (!find_write_stream_char(fp, '\0'))
                    return false;
                break;
            case 'a':
                if (!find_write_stream_char(fp, '\a'))
                    return false;
                break;
            case 'b':
                if (!find_write_stream_char(fp, '\b'))
                    return false;
                break;
            case 'f':
                if (!find_write_stream_char(fp, '\f'))
                    return false;
                break;
            case 'n':
                if (!find_write_stream_char(fp, '\n'))
                    return false;
                break;
            case 'r':
                if (!find_write_stream_char(fp, '\r'))
                    return false;
                break;
            case 't':
                if (!find_write_stream_char(fp, '\t'))
                    return false;
                break;
            case 'v':
                if (!find_write_stream_char(fp, '\v'))
                    return false;
                break;
            case 'c':
                return true;
            default:
                if (!find_write_stream_char(fp, '\\') ||
                    !find_write_stream_char(fp, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (format[i] == '%') {
            i++;
            if (format[i] == '\0')
                return find_write_stream_char(fp, '%');
            switch (format[i]) {
            case '%':
                if (!find_write_stream_char(fp, '%'))
                    return false;
                break;
            case 'p':
                if (!find_write_stream_bytes(fp, entry->path, strlen(entry->path)))
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
                bool ok = find_write_stream_bytes(fp, target, strlen(target));
                free(target);
                if (!ok)
                    return false;
                break;
            }
            case 'm': {
                if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_OUTPUT))
                    return false;
                if (fprintf(fp, "%o", entry->mode & 07777u) < 0)
                    return false;
                break;
            }
            case 'h': {
                char *dir = bx_path_dirname_dup(entry->path);
                if (!dir)
                    return false;
                bool ok = find_write_stream_bytes(fp, dir, strlen(dir));
                free(dir);
                if (!ok)
                    return false;
                break;
            }
            default:
                if (!find_write_stream_char(fp, '%') ||
                    !find_write_stream_char(fp, format[i]))
                    return false;
                break;
            }
            continue;
        }

        if (!find_write_stream_char(fp, format[i]))
            return false;
    }

    return true;
}

static const char *find_user_name(uid_t uid, char numeric_buffer[32]) {
    return bx_id_user_name(uid, numeric_buffer);
}

static const char *find_group_name(gid_t gid, char numeric_buffer[32]) {
    return bx_id_group_name(gid, numeric_buffer);
}

bool find_write_ls_entry(FILE *fp, const struct bx_walk_entry *entry) {
    if (!fp || !entry)
        return false;

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

    if (fprintf(fp, "%10" PRIuMAX " %6" PRIuMAX " %s %3" PRIuMAX " %-8s %-8s ",
                (uintmax_t)display->st_ino, blocks, mode,
                (uintmax_t)display->st_nlink, user_name, group_name) < 0) {
        return false;
    }

    if (S_ISCHR(display->st_mode) || S_ISBLK(display->st_mode)) {
        if (fprintf(fp, "%3" PRIuMAX ", %3" PRIuMAX " %s %s",
                    (uintmax_t)major(display->st_rdev),
                    (uintmax_t)minor(display->st_rdev),
                    timestamp, entry->path) < 0) {
            return false;
        }
    } else if (fprintf(fp, "%8jd %s %s",
                       (intmax_t)display->st_size, timestamp, entry->path) < 0) {
        return false;
    }

    if (have_lstat && S_ISLNK(lst.st_mode) && (!entry->follow_metadata || !have_stat)) {
        char *link_target = bx_path_readlink_dup(entry->path);
        if (link_target) {
            bool ok = fprintf(fp, " -> %s", link_target) >= 0;
            free(link_target);
            if (!ok)
                return false;
        }
    }

    return fputc('\n', fp) != EOF;
}
