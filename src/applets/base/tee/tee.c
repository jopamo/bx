#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/xreadwrite.h"
#include "lib/cli_common.h"
#include "lib/fd_ops.h"
#include "lib/line_writer.h"
#include "lib/option_scan.h"

struct tee_output {
    int fd;
    const char* name;
    bool close_fd;
    bool failed;
    struct bx_line_writer writer;
};

static void tee_print_help(const char* progname) {
    printf("Usage: %s [OPTION]... [FILE]...\n", progname);
    printf("Copy standard input to each FILE, and also to standard output.\n");
    printf("\n");
    printf("  -a, --append             append to the given FILEs, do not overwrite\n");
    printf("  -i, --ignore-interrupts  ignore interrupt signals\n");
    printf("      --help          display this help and exit\n");
    printf("      --version       output version information and exit\n");
}

int bx_tee_main(int argc, char** argv) {
    static const struct bx_option_spec specs[] = {
        {'a', 'a', "append", BX_OPTION_NO_VALUE},
        {'i', 'i', "ignore-interrupts", BX_OPTION_NO_VALUE},
        {'h', 0, "help", BX_OPTION_NO_VALUE},
        {'v', 0, "version", BX_OPTION_NO_VALUE},
    };

    const char* progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "tee");
    struct bx_diag_ctx diag = {.progname = progname, .exit_status = 0};
    bool append = false;
    bool ignore_interrupts = false;
    /* At most argc - 1 operands, plus the standard output entry. */
    const char** paths = calloc((size_t)argc, sizeof(*paths));
    if (paths == NULL) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }
    int num_files = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            paths[num_files++] = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            free(paths);
            return diag.exit_status;
        }
        switch (option.spec->id) {
            case 'a':
                append = true;
                break;
            case 'i':
                ignore_interrupts = true;
                break;
            case 'h':
                tee_print_help(progname);
                free(paths);
                return 0;
            case 'v':
                printf("tee (bx) %s\n", BX_VERSION);
                free(paths);
                return 0;
        }
    }

    for (int i = 0; i < num_files; i++) {
        if (paths[i][0] == '\0') {
            bx_diag(&diag, "empty file operand");
            free(paths);
            return diag.exit_status;
        }
    }
    if (ignore_interrupts && signal(SIGINT, SIG_IGN) == SIG_ERR) {
        bx_perror_path(&diag, "cannot ignore SIGINT");
        free(paths);
        return diag.exit_status;
    }

    struct tee_output* outputs = calloc((size_t)num_files + 1u, sizeof(*outputs));
    if (!outputs) {
        bx_diag(&diag, "memory exhausted");
        free(paths);
        return diag.exit_status;
    }

    char* output_buffers = calloc((size_t)num_files + 1u, 8192u);
    if (!output_buffers) {
        bx_diag(&diag, "memory exhausted");
        free(outputs);
        free(paths);
        return diag.exit_status;
    }

    outputs[0] = (struct tee_output){
        .fd = STDOUT_FILENO,
        .name = "standard output",
        .close_fd = false,
        .failed = false,
    };
    bx_line_writer_init(&outputs[0].writer, outputs[0].fd, output_buffers, 8192u);
    int output_count = 1;

    for (int i = 0; i < num_files; i++) {
        const char* path = paths[i];
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = bx_fd_open_cloexec(path, flags, 0666);
        if (fd >= 0) {
            int output_index = output_count;
            outputs[output_index] = (struct tee_output){
                .fd = fd,
                .name = path,
                .close_fd = true,
                .failed = false,
            };
            bx_line_writer_init(&outputs[output_index].writer,
                                outputs[output_index].fd,
                                output_buffers + ((size_t)output_index * 8192u),
                                8192u);
            output_count++;
        }
        else {
            bx_perror_path(&diag, path);
        }
    }
    free(paths);

    char buf[8192];
    ssize_t n;
    while ((n = bx_xread(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < output_count; i++) {
            if (outputs[i].failed) {
                continue;
            }
            if (!bx_line_writer_write(&outputs[i].writer, buf, (size_t)n)) {
                bx_perror_path(&diag, outputs[i].name);
                outputs[i].failed = true;
            }
        }
    }
    if (n < 0) {
        bx_perror_path(&diag, "standard input");
    }

    for (int i = 0; i < output_count; i++) {
        if (outputs[i].failed) {
            continue;
        }
        if (bx_line_writer_error(&outputs[i].writer) == 0 &&
            !bx_line_writer_flush(&outputs[i].writer)) {
            bx_perror_path(&diag, outputs[i].name);
            outputs[i].failed = true;
        }
    }

    for (int i = 1; i < output_count; i++) {
        if (!outputs[i].close_fd) {
            continue;
        }
        if (outputs[i].failed) {
            bx_fd_cleanup(&outputs[i].fd);
        }
        else if (!bx_fd_close(&outputs[i].fd, outputs[i].name, &diag)) {
            outputs[i].failed = true;
        }
    }
    free(output_buffers);
    free(outputs);

    return diag.exit_status;
}
