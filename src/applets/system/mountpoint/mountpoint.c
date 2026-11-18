#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "applets.h"
#include "lib/args_common.h"
#include "lib/fd_ops.h"
#include "lib/cli_common.h"
#include "lib/statx_compat.h"
#include "lib/mount_table.h"
#include "lib/path_ops.h"

#ifndef STATX_ATTR_MOUNT_ROOT
#define STATX_ATTR_MOUNT_ROOT 0x2000
#endif

static void print_help(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-q] PATH\nTest whether PATH is the root of a mount.\n", name);
}

/* Match both the path and its visible mount: a covered mount can still
 * appear in mountinfo at a path below the covering mount. */
static int lookup_mount(int fd) {
    char proc_path[64];
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fdinfo/%d", fd);
    FILE* fp = fopen(proc_path, "r");
    if (!fp)
        return -1;
    unsigned long mount_id;
    char field[128];
    bool found = false;
    while (fgets(field, sizeof(field), fp)) {
        if (sscanf(field, "mnt_id: %lu", &mount_id) == 1) {
            found = true;
            break;
        }
    }
    int err = ferror(fp) ? errno : EOPNOTSUPP;
    fclose(fp);
    if (!found) {
        errno = err;
        return -1;
    }
    snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    char* path = bx_path_readlink_dup(proc_path);
    if (!path)
        return -1;
    fp = fopen("/proc/self/mountinfo", "r");
    int result = -1;
    if (fp) {
        char* line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, fp) >= 0) {
            unsigned long id;
            int start = 0;
            if (sscanf(line, "%lu %*s %*s %*s %n", &id, &start) != 1 ||
                !start || id != mount_id)
                continue;
            line[start + strcspn(line + start, " ")] = '\0';
            char* target = bx_mount_unescape_field(line + start);
            result = strcmp(path, target) == 0;
            free(target);
            break;
        }
        err = feof(fp) ? EOPNOTSUPP : errno;
        free(line);
        fclose(fp);
    }
    else
        err = errno;
    free(path);
    errno = err;
    return result;
}

static int is_mountpoint(const char* path) {
    struct statx info;
    if (statx(AT_FDCWD, path, 0, STATX_TYPE, &info) == 0) {
        if (info.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)
            return (info.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0;
    }
    else if (errno != ENOSYS && errno != EOPNOTSUPP && errno != EINVAL)
        return -1;
    int fd = bx_fd_open_cloexec(path, O_PATH, 0);
    if (fd < 0)
        return -1;
    int result = lookup_mount(fd);
    int err = errno;
    close(fd);
    errno = err;
    return result;
}

int bx_mountpoint_main(int argc, char** argv) {
    int help = bx_cli_maybe_handle_help_or_version(argc, argv, "mountpoint", NULL, NULL, print_help);
    if (help >= 0)
        return help;
    bool quiet = false;
    bx_args_getopt_reset();
    int opt;
    while ((opt = bx_args_getopt_long(argc, argv, "+q", NULL, NULL)) != -1) {
        if (opt != 'q') {
            print_help(stderr, "mountpoint");
            return 1;
        }
        quiet = true;
    }
    if (argc - optind != 1) {
        print_help(stderr, "mountpoint");
        return 1;
    }
    const char* path = argv[optind];
    int mounted = is_mountpoint(path);
    if (mounted < 0) {
        if (!quiet)
            fprintf(stderr, "mountpoint: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!quiet)
        printf("%s is %sa mountpoint\n", path, mounted ? "" : "not ");
    return mounted ? 0 : 32;
}
