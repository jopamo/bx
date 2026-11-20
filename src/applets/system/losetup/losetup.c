#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <dirent.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/loop.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "lib/fd_ops.h"
#include "lib/loop_device.h"
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

int bx_losetup_main(int argc, char** argv) {
    static const struct option long_options[] = {
        {"find", no_argument, NULL, 'f'}, {"show", no_argument, NULL, 256},
        {"detach", no_argument, NULL, 'd'}, {"read-only", no_argument, NULL, 'r'},
        {"offset", required_argument, NULL, 'o'}, {"sizelimit", required_argument, NULL, 257},
        {"partscan", no_argument, NULL, 'P'}, {"sector-size", required_argument, NULL, 258},
        {"list", no_argument, NULL, 'a'}, {"all", no_argument, NULL, 'a'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0},
    };
    struct bx_diag_ctx diag = {.progname = "losetup"};
    struct loop_options opts = {0};
    bool settings = false;
    int ch;
    while ((ch = getopt_long(argc, argv, "adfo:Pr", long_options, NULL)) != -1) {
        uintmax_t number;
        switch (ch) {
            case 'f': opts.find = true; break;
            case 'd': opts.detach = true; break;
            case 'a': opts.list = true; break;
            case 256: opts.show = true; break;
            case 'r': opts.info.lo_flags |= LO_FLAGS_READ_ONLY; settings = true; break;
            case 'P': opts.info.lo_flags |= LO_FLAGS_PARTSCAN; settings = true; break;
            case 'o':
            case 257:
            case 258:
                if (!bx_size_parse_scaled_uint(optarg, &number) || number > INT64_MAX ||
                    (ch == 258 && (number < 512 || number > 4096 || (number & (number - 1)) != 0))) {
                    bx_diag(&diag, "invalid size: '%s'", optarg);
                    return 1;
                }
                if (ch == 'o')
                    opts.info.lo_offset = number;
                else if (ch == 257)
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
            default:
                return 1;
        }
    }
#if !defined(LOOP_SET_BLOCK_SIZE) && !defined(LOOP_CONFIGURE)
    if (opts.sector_size != 0) {
        bx_diag(&diag, "logical sector size is unsupported by the build headers");
        return 1;
    }
#endif
    int n = argc - optind;
    if ((opts.find + opts.detach + opts.list > 1) ||
        ((settings || opts.show) && (opts.detach || opts.list)) ||
        (opts.show && (!opts.find || n != 1)) ||
        (opts.list && n != 0) || (opts.find && (n > 1 || (settings && n == 0))) ||
        (opts.detach && n == 0) || (!opts.find && !opts.detach && n > 2) ||
        (settings && !opts.find && n != 2)) {
        bx_diag(&diag, "invalid operation or operand count");
        return 1;
    }
    if (opts.detach) {
        int status = 0;
        for (; optind < argc; optind++)
            status |= detach_loop(argv[optind], &diag);
        return status;
    }
    if (opts.find)
        return attach_loop(NULL, n == 1 ? argv[optind] : NULL, &opts, &diag);
    if (n == 0)
        return list_loops(&diag);
    if (n == 1) {
        int status = report_loop(argv[optind], &diag, false);
        if (fflush(stdout) == EOF) {
            bx_perror_path(&diag, "standard output");
            status = 1;
        }
        return status;
    }
    return attach_loop(argv[optind], argv[optind + 1], &opts, &diag);
}
#else
int bx_losetup_main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    fputs("losetup: Linux loop devices are not supported on this platform\n", stderr);
    return 1;
}
#endif
