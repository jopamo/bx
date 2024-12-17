#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

struct bx_touch_options {
    const char* progname;
    bool update_atime;
    bool update_mtime;
    bool no_create;
    bool show_help;
    bool show_version;
};

static const char* bx_touch_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "touch";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_touch_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... FILE...\n", progname);
    fprintf(stream, "Update the access and modification times of each FILE to now.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a             change only the access time\n");
    fprintf(stream, "  -m             change only the modification time\n");
    fprintf(stream, "  -c, --no-create  do not create any files\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_touch_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_touch_parse_options(int argc, char** argv, struct bx_touch_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"no-create", no_argument, NULL, 'c'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_touch_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+acm", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->update_atime = true;
                break;
            case 'm':
                options->update_mtime = true;
                break;
            case 'c':
                options->no_create = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    if (!options->update_atime && !options->update_mtime) {
        options->update_atime = true;
        options->update_mtime = true;
    }

    *first_operand = optind;
    return true;
}

static const struct timespec* bx_touch_requested_times(const struct bx_touch_options* options, struct timespec times[2]) {
    if (options->update_atime && options->update_mtime) {
        return NULL;
    }

    times[0].tv_sec = 0;
    times[0].tv_nsec = options->update_atime ? UTIME_NOW : UTIME_OMIT;
    times[1].tv_sec = 0;
    times[1].tv_nsec = options->update_mtime ? UTIME_NOW : UTIME_OMIT;
    return times;
}

static void bx_touch_path(const char* path, const struct bx_touch_options* options, struct bx_diag_ctx* diag) {
    struct timespec times[2];
    const struct timespec* requested_times = bx_touch_requested_times(options, times);

    if (utimensat(AT_FDCWD, path, requested_times, 0) == 0) {
        return;
    }

    if (errno != ENOENT) {
        bx_perror_path(diag, path);
        return;
    }

    if (options->no_create) {
        return;
    }

    int fd = open(path, O_WRONLY | O_CREAT, 0666u);
    if (fd < 0) {
        bx_perror_path(diag, path);
        return;
    }

    if (close(fd) != 0) {
        bx_perror_path(diag, path);
        return;
    }

    if (utimensat(AT_FDCWD, path, requested_times, 0) != 0) {
        bx_perror_path(diag, path);
    }
}

int bx_touch_main(int argc, char** argv) {
    struct bx_touch_options options;
    struct bx_diag_ctx diag = {
        .progname = "touch",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_touch_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_touch_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_touch_print_version(options.progname);
        return 0;
    }

    if (first_operand >= argc) {
        bx_diag(&diag, "missing file operand");
        return diag.exit_status;
    }

    for (int i = first_operand; i < argc; i++) {
        bx_touch_path(argv[i], &options, &diag);
    }

    return diag.exit_status;
}
