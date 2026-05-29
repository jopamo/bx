#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <stdbool.h>
#include <signal.h>
#include <fcntl.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/xreadwrite.h"
#include "lib/args_common.h"

struct tee_output {
    int fd;
    const char* name;
    bool close_fd;
    bool failed;
};

static const char* tee_basename(const char* path) {
    if (!path || !*path) {
        return "tee";
    }

    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

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
    static const struct option long_options[] = {
        {"append", no_argument, NULL, 'a'}, {"ignore-interrupts", no_argument, NULL, 'i'}, {"help", no_argument, NULL, 'h'}, {"version", no_argument, NULL, 'v'}, {NULL, 0, NULL, 0}};

    const char* progname = tee_basename(argc > 0 ? argv[0] : "tee");
    struct bx_diag_ctx diag = {.progname = progname, .exit_status = 0};
    bool append = false;
    bool ignore_interrupts = false;
    int c;
    bx_args_getopt_reset();
    while ((c = bx_args_getopt_long(argc, argv, "ai", long_options, NULL)) != -1) {
        switch (c) {
            case 'a':
                append = true;
                break;
            case 'i':
                ignore_interrupts = true;
                break;
            case 'h':
                tee_print_help(progname);
                return 0;
            case 'v':
                printf("tee (bx) %s\n", BX_VERSION);
                return 0;
            default:
                if (optopt != 0) {
                    fprintf(stderr, "%s: invalid option -- '%c'\n", progname, optopt);
                }
                else if (optind > 0 && argv[optind - 1]) {
                    fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
                }
                else {
                    fprintf(stderr, "%s: invalid option\n", progname);
                }
                return 1;
        }
    }

    if (ignore_interrupts) {
        signal(SIGINT, SIG_IGN);
    }

    int num_files = argc - optind;
    struct tee_output* outputs = calloc((size_t)num_files + 1u, sizeof(*outputs));
    if (!outputs) {
        bx_diag(&diag, "memory exhausted");
        return diag.exit_status;
    }

    outputs[0] = (struct tee_output){
        .fd = STDOUT_FILENO,
        .name = "standard output",
        .close_fd = false,
        .failed = false,
    };
    int output_count = 1;

    for (int i = 0; i < num_files; i++) {
        const char* path = argv[optind + i];
        int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        int fd = open(path, flags, 0666);
        if (fd >= 0) {
            outputs[output_count++] = (struct tee_output){
                .fd = fd,
                .name = path,
                .close_fd = true,
                .failed = false,
            };
        }
        else {
            bx_perror_path(&diag, path);
        }
    }

    char buf[8192];
    ssize_t n;
    while ((n = bx_xread(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < output_count; i++) {
            if (outputs[i].failed) {
                continue;
            }
            if (!bx_xwrite_all(outputs[i].fd, buf, (size_t)n)) {
                bx_perror_path(&diag, outputs[i].name);
                outputs[i].failed = true;
            }
        }
    }
    if (n < 0) {
        bx_perror_path(&diag, "standard input");
    }

    for (int i = 1; i < output_count; i++) {
        if (outputs[i].close_fd && close(outputs[i].fd) != 0 && !outputs[i].failed) {
            bx_perror_path(&diag, outputs[i].name);
        }
    }
    free(outputs);

    return diag.exit_status;
}
