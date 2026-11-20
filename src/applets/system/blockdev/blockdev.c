#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"

#ifdef __linux__
#include <inttypes.h>
#include <linux/fs.h>
#include <sys/ioctl.h>

#include "lib/fd_ops.h"

static const struct {
    const char* option;
    unsigned long request;
} commands[] = {
    {"--getsize64", BLKGETSIZE64},
    {"--getsz", BLKGETSIZE64},
    {"--getss", BLKSSZGET},
    {"--getpbsz", BLKPBSZGET},
    {"--getro", BLKROGET},
    {"--rereadpt", BLKRRPART},
};

static unsigned long blockdev_request(const char* option) {
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        if (strcmp(option, commands[i].option) == 0)
            return commands[i].request;
    }
    return 0;
}

static bool blockdev_operation(const char* option, int fd, const char* path,
                               struct bx_diag_ctx* diag) {
    unsigned long request = blockdev_request(option);
    int printed;
    switch (request) {
    case BLKRRPART:
        if (ioctl(fd, request, 0UL) == 0)
            return true;
        goto ioctl_error;
    case BLKGETSIZE64: {
        uint64_t bytes;
        if (ioctl(fd, request, &bytes) < 0)
            goto ioctl_error;
        /* --getsz uses 512-byte units even with larger logical sectors. */
        printed = printf("%" PRIu64 "\n", strcmp(option, "--getsz") == 0 ? bytes / 512 : bytes);
        break;
    }
    case BLKPBSZGET: {
        unsigned int size;
        if (ioctl(fd, request, &size) < 0)
            goto ioctl_error;
        printed = printf("%u\n", size);
        break;
    }
    default: { /* BLKROGET, BLKSSZGET */
        int value;
        if (ioctl(fd, request, &value) < 0)
            goto ioctl_error;
        printed = printf("%d\n", value);
        break;
    }
    }
    if (printed < 0 || fflush(stdout) == EOF) {
        bx_perror_path(diag, "standard output");
        return false;
    }
    return true;

ioctl_error:
    bx_diag(diag, "%s: %s: %s", path, option, bx_strerror(errno));
    return false;
}

static bool blockdev_run(const char* path, int ncommands, char** options, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    bool success = false;
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0) {
        bx_perror_path(diag, path);
    } else if (!S_ISBLK(st.st_mode)) {
        bx_diag(diag, "%s: not a block device", path);
    } else {
        success = true;
        for (int i = 0; i < ncommands; i++) {
            if (!blockdev_operation(options[i], fd, path, diag)) {
                success = false;
                break;
            }
        }
    }
    return bx_fd_close(&fd, success ? path : NULL, diag) && success;
}
#endif

int bx_blockdev_main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("usage: blockdev COMMAND... [--] DEVICE...\n"
             "Commands: --getsize64 --getsz --getss --getpbsz --getro --rereadpt");
        return 0;
    }
    struct bx_diag_ctx diag = {.progname = "blockdev"};
#ifdef __linux__
    int end = 1;
    /* Validate option syntax before opening any device. */
    while (end < argc && argv[end][0] == '-' && strcmp(argv[end], "--") != 0) {
        if (blockdev_request(argv[end]) == 0) {
            bx_diag(&diag, "unsupported command: '%s'", argv[end]);
            return 1;
        }
        end++;
    }
    bool separator = end < argc && strcmp(argv[end], "--") == 0;
    int first_device = end + separator;
    if (end == 1 || first_device >= argc) {
        bx_diag(&diag, "a command and a block device are required");
        return 1;
    }
    for (int i = first_device; i < argc; i++) {
        if (!separator && argv[i][0] == '-') {
            bx_diag(&diag, "unexpected option after device: '%s'", argv[i]);
            return 1;
        }
        if (argv[i][0] == '\0') {
            bx_diag(&diag, "empty device pathname");
            return 1;
        }
    }
    for (int i = first_device; i < argc; i++)
        if (!blockdev_run(argv[i], end - 1, argv + 1, &diag))
            return 1;
    return 0;
#else
    bx_diag(&diag, "Linux block device operations are not supported on this platform");
    return 1;
#endif
}
