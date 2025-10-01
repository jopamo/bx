#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include "find_output.h"
#include "search/walk.h"

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
                              const struct walk_entry *entry) {
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

static char find_mode_type_char(mode_t mode) {
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

static void find_mode_to_string(mode_t mode, char out[11]) {
    out[0] = find_mode_type_char(mode);
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

static const char *find_user_name(uid_t uid, char numeric_buffer[32]) {
    struct passwd *pw = getpwuid(uid);
    if (pw && pw->pw_name && pw->pw_name[0] != '\0')
        return pw->pw_name;
    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

static const char *find_group_name(gid_t gid, char numeric_buffer[32]) {
    struct group *gr = getgrgid(gid);
    if (gr && gr->gr_name && gr->gr_name[0] != '\0')
        return gr->gr_name;
    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}

static void find_format_timestamp(time_t timestamp, char buffer[32]) {
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

    const char *fmt =
        (delta > (365.0 / 2.0) * 24.0 * 60.0 * 60.0 || timestamp > now + 3600)
            ? "%b %e  %Y"
            : "%b %e %H:%M";
    if (strftime(buffer, 32, fmt, &tm_value) == 0)
        snprintf(buffer, 32, "??? ?? ??:??");
}

bool find_write_ls_entry(FILE *fp, const struct walk_entry *entry) {
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
    find_mode_to_string(display->st_mode, mode);
    const char *user_name = find_user_name(display->st_uid, user_numeric);
    const char *group_name = find_group_name(display->st_gid, group_numeric);
    find_format_timestamp(display->st_mtime, timestamp);

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
        char link_target[PATH_MAX + 1];
        ssize_t len = readlink(entry->path, link_target, PATH_MAX);
        if (len >= 0) {
            link_target[len] = '\0';
            if (fprintf(fp, " -> %s", link_target) < 0)
                return false;
        }
    }

    return fputc('\n', fp) != EOF;
}
