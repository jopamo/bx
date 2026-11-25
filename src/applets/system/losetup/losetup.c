#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <dirent.h>
#include <inttypes.h>
#include <linux/loop.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "lib/fd_ops.h"
#include "lib/loop_device.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"

struct loop_options {
    struct loop_info64 info;
    unsigned sector_size;
    bool find, show, list, detach;
};

static int report_loop(const char* path, struct bx_diag_ctx* diag, bool listing) {
    struct loop_info64 info;
    int fd = bx_loop_open(path, O_RDONLY);
    if (fd >= 0 && ioctl(fd, LOOP_GET_STATUS64, &info) == 0) {
        bx_fd_cleanup(&fd);
        if (printf("%s: [%04" PRIx64 "]:%" PRIu64 " (%.*s)\n", path,
                   (uint64_t)info.lo_device, (uint64_t)info.lo_inode,
                   (int)sizeof(info.lo_file_name), (const char*)info.lo_file_name) >= 0)
            return 0;
        bx_perror_path(diag, "standard output");
        return 1;
    }
    int error = errno;
    bx_fd_cleanup(&fd);
    if (listing && (error == ENOENT || error == ENXIO))
        return 0;
    errno = error;
    bx_perror_path(diag, path);
    return 1;
}

static int list_loops(struct bx_diag_ctx* diag) {
    DIR* dir = opendir("/sys/block");
    if (dir == NULL) {
        bx_perror_path(diag, "/sys/block");
        return 1;
    }
    int status = 0;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_perror_path(diag, "/sys/block");
                status = 1;
            }
            break;
        }
        const char* name = entry->d_name;
        if (strncmp(name, "loop", 4) != 0 || name[4] == '\0' ||
            name[4 + strspn(name + 4, "0123456789")] != '\0')
            continue;
        char path[64];
        int n = snprintf(path, sizeof(path), "/dev/%s", name);
        if (n > 0 && (size_t)n < sizeof(path))
            status |= report_loop(path, diag, true);
    }
    closedir(dir);
    if (fflush(stdout) == EOF) {
        bx_perror_path(diag, "standard output");
        status = 1;
    }
    return status;
}

static int detach_loop(const char* path, struct bx_diag_ctx* diag) {
    int fd = bx_loop_open(path, O_RDONLY);
    int status = fd >= 0 && ioctl(fd, LOOP_CLR_FD) == 0 ? 0 : 1;
    if (status != 0)
        bx_perror_path(diag, path);
    bx_fd_cleanup(&fd);
    return status;
}

static int configure_loop(int fd, int backing, const struct loop_options* options, bool* owned) {
#ifdef LOOP_CONFIGURE
    struct loop_config config = {
        .fd = (uint32_t)backing,
        .block_size = options->sector_size,
        .info = options->info,
    };
    if (ioctl(fd, LOOP_CONFIGURE, &config) == 0) {
        *owned = true;
        return 0;
    }
    if (errno != ENOTTY && errno != ENOSYS) {
        if (errno != EINVAL)
            return -1;
        /* Old loop drivers used EINVAL for unknown ioctls. A driver with
         * LOOP_CONFIGURE checks the pointer before it can mutate anything. */
        if (ioctl(fd, LOOP_CONFIGURE, NULL) == 0 ||
            (errno != EINVAL && errno != ENOTTY && errno != ENOSYS)) {
            errno = EINVAL;
            return -1;
        }
    }
#endif
    if (ioctl(fd, LOOP_SET_FD, backing) < 0)
        return -1;
    *owned = true;
#ifdef LOOP_SET_BLOCK_SIZE
    if (options->sector_size != 0 && ioctl(fd, LOOP_SET_BLOCK_SIZE, (unsigned long)options->sector_size) < 0)
        return -1;
#endif
    return ioctl(fd, LOOP_SET_STATUS64, &options->info);
}

static int attach_loop(const char* device, const char* file, struct loop_options* options, struct bx_diag_ctx* diag) {
    int status = 1, control = -1, backing = -1;
    if (file != NULL) {
        int access = (options->info.lo_flags & LO_FLAGS_READ_ONLY) != 0 ? O_RDONLY : O_RDWR;
        backing = bx_fd_open_cloexec(file, access | O_NONBLOCK | O_NOCTTY, 0);
        if (backing < 0) {
            bx_perror_path(diag, file);
            goto out;
        }
        struct stat st;
        if (fstat(backing, &st) < 0) {
            bx_perror_path(diag, file);
            goto out;
        }
        if (!S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) {
            bx_diag(diag, "backing path is not a regular file or block device: '%s'", file);
            goto out;
        }
        snprintf((char*)options->info.lo_file_name, sizeof(options->info.lo_file_name), "%s", file);
    }
    if (options->find) {
        control = bx_fd_open_cloexec("/dev/loop-control", O_RDWR, 0);
        if (control < 0) {
            bx_perror_path(diag, "/dev/loop-control");
            goto out;
        }
    }

    char selected[64];
    for (unsigned int attempt = 0; attempt < 32; attempt++) {
        if (options->find) {
            int number = ioctl(control, LOOP_CTL_GET_FREE);
            if (number < 0) {
                bx_perror_path(diag, "/dev/loop-control");
                break;
            }
            snprintf(selected, sizeof(selected), "/dev/loop%d", number);
            device = selected;
        }
        if (file == NULL) {
            status = puts(device) < 0 || fflush(stdout) == EOF;
            if (status != 0)
                bx_perror_path(diag, "standard output");
            break;
        }
        int fd = bx_loop_open(device, O_RDWR | O_EXCL);
        if (fd < 0) {
            if (options->find && attempt + 1 < 32 && (errno == EBUSY || errno == ENOENT)) {
                (void)nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
                continue;
            }
            bx_perror_path(diag, device);
            break;
        }

        /* Keep catchable cancellation and SIGPIPE out of legacy setup and
         * rollback. The open exclusive claim spans all setup ioctls. */
        sigset_t blocked, previous;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGTERM);
        sigaddset(&blocked, SIGINT);
        sigaddset(&blocked, SIGHUP);
        sigaddset(&blocked, SIGPIPE);
        if (sigprocmask(SIG_BLOCK, &blocked, &previous) < 0) {
            bx_perror_path(diag, "sigprocmask");
            bx_fd_cleanup(&fd);
            break;
        }
        bool owned = false;
        int result = configure_loop(fd, backing, options, &owned);
        int error = errno;
        bool retry = result < 0 && !owned && error == EBUSY && options->find && attempt + 1 < 32;
        if (result == 0 && options->show && (puts(device) < 0 || fflush(stdout) == EOF)) {
            bx_perror_path(diag, "standard output");
            result = -1;
        } else if (result < 0 && !retry) {
            errno = error;
            bx_perror_path(diag, device);
        }
        if (result < 0 && owned && ioctl(fd, LOOP_CLR_FD) < 0)
            bx_diag(diag, "cannot roll back association for '%s': %s", device, strerror(errno));
        bx_fd_cleanup(&fd);
        (void)sigprocmask(SIG_SETMASK, &previous, NULL);
        if (retry) {
            (void)nanosleep(&(struct timespec){.tv_nsec = 10000000}, NULL);
            continue;
        }
        status = result < 0;
        break;
    }
out:
    bx_fd_cleanup(&backing);
    bx_fd_cleanup(&control);
    return status;
}

static int loop_command(int argc, char** argv, const char** operands, struct bx_diag_ctx* diag) {
    enum { SHOW = 256, SIZELIMIT, SECTOR_SIZE };
    static const struct bx_option_spec options[] = {
        {'f', 'f', "find", BX_OPTION_NO_VALUE},
        {SHOW, 0, "show", BX_OPTION_NO_VALUE},
        {'d', 'd', "detach", BX_OPTION_NO_VALUE},
        {'r', 'r', "read-only", BX_OPTION_NO_VALUE},
        {'o', 'o', "offset", BX_OPTION_REQUIRED_VALUE},
        {SIZELIMIT, 0, "sizelimit", BX_OPTION_REQUIRED_VALUE},
        {'P', 'P', "partscan", BX_OPTION_NO_VALUE},
        {SECTOR_SIZE, 0, "sector-size", BX_OPTION_REQUIRED_VALUE},
        {'a', 'a', "list", BX_OPTION_NO_VALUE},
        {'a', 0, "all", BX_OPTION_NO_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
    };
    struct loop_options opts = {0};
    bool settings = false;
    size_t n = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ABBREVIATE,
                          options, sizeof(options) / sizeof(options[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            operands[n++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE && result.spec->short_name)
                bx_diag(diag, "option requires an argument -- '%c'", result.spec->short_name);
            else if (result.event == BX_OPTION_MISSING_VALUE)
                bx_diag(diag, "option requires an argument -- '%s'", result.token);
            else if (result.short_name)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return 1;
        }
        uintmax_t number;
        int ch = result.spec->id;
        switch (ch) {
            case 'f': opts.find = true; break;
            case 'd': opts.detach = true; break;
            case 'a': opts.list = true; break;
            case SHOW: opts.show = true; break;
            case 'r': opts.info.lo_flags |= LO_FLAGS_READ_ONLY; settings = true; break;
            case 'P': opts.info.lo_flags |= LO_FLAGS_PARTSCAN; settings = true; break;
            case 'o':
            case SIZELIMIT:
            case SECTOR_SIZE:
                if (!bx_size_parse_scaled_uint(result.value, &number) || number > INT64_MAX ||
                    (ch == SECTOR_SIZE && (number < 512 || number > 4096 || (number & (number - 1)) != 0))) {
                    bx_diag(diag, "invalid size: '%s'", result.value);
                    return 1;
                }
                if (ch == 'o')
                    opts.info.lo_offset = number;
                else if (ch == SIZELIMIT)
                    opts.info.lo_sizelimit = number;
                else
                    opts.sector_size = (unsigned)number;
                settings = true;
                break;
            case 'h':
                puts("usage: losetup [-a | -d LOOPDEV... | LOOPDEV]\n"
                     "       losetup -f [--show] [-rP] [-o OFFSET] [--sizelimit SIZE] [--sector-size BYTES] [FILE]\n"
                     "       losetup [-rP] [-o OFFSET] [--sizelimit SIZE] [--sector-size BYTES] LOOPDEV FILE\n"
                     "Whole loop devices are verified through /sys.");
                return 0;
        }
    }
#if !defined(LOOP_SET_BLOCK_SIZE) && !defined(LOOP_CONFIGURE)
    if (opts.sector_size != 0) {
        bx_diag(diag, "logical sector size is unsupported by the build headers");
        return 1;
    }
#endif
    if ((opts.find + opts.detach + opts.list > 1) ||
        ((settings || opts.show) && (opts.detach || opts.list)) ||
        (opts.show && (!opts.find || n != 1)) ||
        (opts.list && n != 0) || (opts.find && (n > 1 || (settings && n == 0))) ||
        (opts.detach && n == 0) || (!opts.find && !opts.detach && n > 2) ||
        (settings && !opts.find && n != 2)) {
        bx_diag(diag, "invalid operation or operand count");
        return 1;
    }
    for (size_t i = 0; i < n; i++) {
        if (!*operands[i]) {
            bx_diag(diag, "empty path operand");
            return 1;
        }
    }
    if (opts.detach) {
        int status = 0;
        for (size_t i = 0; i < n; i++)
            status |= detach_loop(operands[i], diag);
        return status;
    }
    if (opts.find)
        return attach_loop(NULL, n == 1 ? operands[0] : NULL, &opts, diag);
    if (n == 0)
        return list_loops(diag);
    if (n == 1) {
        int status = report_loop(operands[0], diag, false);
        if (fflush(stdout) == EOF) {
            bx_perror_path(diag, "standard output");
            status = 1;
        }
        return status;
    }
    return attach_loop(operands[0], operands[1], &opts, diag);
}
#endif

int bx_losetup_main(int argc, char** argv) {
#ifdef __linux__
    struct bx_diag_ctx diag = {.progname = "losetup"};
    const char** operands = NULL;
    if (argc > 1) {
        operands = calloc((size_t)argc - 1, sizeof(*operands));
        if (!operands) {
            bx_perror_path(&diag, "path operands");
            return 1;
        }
    }
    int status = loop_command(argc, argv, operands, &diag);
    free(operands);
    return status;
#else
    (void)argc;
    (void)argv;
    fputs("losetup: Linux loop devices are not supported on this platform\n", stderr);
    return 1;
#endif
}
