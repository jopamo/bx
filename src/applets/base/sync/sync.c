#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

struct bx_sync_options {
    const char* progname;
    bool data_only;
    bool file_system;
    bool show_help;
    bool show_version;
};

static void bx_sync_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION] [FILE]...\n", progname);
    fprintf(stream, "Synchronize cached writes to persistent storage.\n");
    fprintf(stream, "\n");
    fprintf(stream, "If one or more files are specified, sync only them,\n");
    fprintf(stream, "or their containing file systems.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -d, --data         sync only file data, no unneeded metadata\n");
    fprintf(stream, "  -f, --file-system  sync the file systems that contain the files\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n");
}

static bool bx_sync_parse_options(int argc, char** argv, struct bx_sync_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"data", no_argument, NULL, 'd'}, {"file-system", no_argument, NULL, 'f'}, {"help", no_argument, NULL, 1}, {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sync");
    diag->progname = options->progname;

    bx_args_getopt_reset();

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+df", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'd':
                options->data_only = true;
                break;
            case 'f':
                options->file_system = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static int bx_sync_syncfs_fd(int fd) {
#ifdef SYS_syncfs
    return (int)syscall(SYS_syncfs, fd);
#else
    (void)fd;
    errno = ENOSYS;
    return -1;
#endif
}

static void bx_sync_one_path(const char* path, const struct bx_sync_options* options, struct bx_diag_ctx* diag) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        bx_diag(diag, "error opening '%s': %s", path, strerror(errno));
        return;
    }

    int rc = 0;
    if (options->data_only) {
        rc = fdatasync(fd);
    }
    else if (options->file_system) {
        rc = bx_sync_syncfs_fd(fd);
        if (rc != 0 && errno == ENOSYS) {
            rc = fsync(fd);
        }
    }
    else {
        rc = fsync(fd);
    }

    if (rc != 0) {
        bx_diag(diag, "error syncing '%s': %s", path, strerror(errno));
    }

    if (close(fd) != 0) {
        bx_diag(diag, "error closing '%s': %s", path, strerror(errno));
    }
}

int bx_sync_main(int argc, char** argv) {
    struct bx_sync_options options;
    struct bx_diag_ctx diag = {
        .progname = "sync",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_sync_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_sync_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    char** operands = argv + first_operand;

    if (options.data_only && options.file_system) {
        bx_diag(&diag, "cannot specify both --data and --file-system");
        return diag.exit_status;
    }

    if (options.data_only && operand_count == 0) {
        bx_diag(&diag, "--data needs at least one argument");
        return diag.exit_status;
    }

    if (operand_count == 0) {
        sync();
        return 0;
    }

    for (int i = 0; i < operand_count; i++) {
        bx_sync_one_path(operands[i], &options, &diag);
    }

    return diag.exit_status;
}
