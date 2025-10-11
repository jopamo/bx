#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>

#include "lib/file_info_fmt.h"

char bx_file_mode_type_char(mode_t mode) {
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

void bx_file_mode_to_string(mode_t mode, char out[11]) {
    out[0] = bx_file_mode_type_char(mode);
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

void bx_file_format_ls_timestamp(time_t timestamp, char buffer[32]) {
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
