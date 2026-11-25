#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/option_scan.h"

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
    enum { OPT_HELP = 256, OPT_VERSION };
    static const struct bx_option_spec specs[] = {
        {'d', 'd', "data", BX_OPTION_NO_VALUE},
        {'f', 'f', "file-system", BX_OPTION_NO_VALUE},
        {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "sync");
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }

        switch (option.spec->id) {
            case 'd':
                options->data_only = true;
                break;
            case 'f':
                options->file_system = true;
                break;
            case OPT_HELP:
                options->show_help = true;
                return true;
            case OPT_VERSION:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    *first_operand = (int)option.index;
    return true;
}

static void bx_sync_one_path(const char* path, const struct bx_sync_options* options, struct bx_diag_ctx* diag) {
    int fd = bx_fd_open_cloexec(path, O_RDONLY | O_NONBLOCK, 0);
    if (fd < 0) {
        bx_diag(diag, "error opening '%s': %s", path, strerror(errno));
        return;
    }

    int rc = 0;
    if (options->data_only) {
        rc = bx_fd_fdatasync(fd);
    }
    else if (options->file_system) {
        rc = bx_fd_syncfs(fd);
        if (rc != 0 && errno == ENOSYS) {
            rc = bx_fd_fsync(fd);
        }
    }
    else {
        rc = bx_fd_fsync(fd);
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

    for (int i = 0; i < operand_count; i++) {
        if (operands[i][0] == '\0') {
            bx_diag(&diag, "empty file operand");
            return diag.exit_status;
        }
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
