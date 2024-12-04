#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "applets.h"
#include "diag.h"

struct bx_mknod_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_mknod_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mknod";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_mknod_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NAME TYPE [MAJOR MINOR]\n", progname);
    fprintf(stream, "Create the special file NAME of the given TYPE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "TYPE may be one of:\n");
    fprintf(stream, "  b  create a block (buffered) special file\n");
    fprintf(stream, "  c, u  create a character (unbuffered) special file\n");
    fprintf(stream, "  p  create a FIFO\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_mknod_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_mknod_parse_options(int argc, char** argv, struct bx_mknod_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mknod_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
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

    *first_operand = optind;
    return true;
}

static bool bx_mknod_parse_type(const char* type_text, mode_t* type_mode, bool* need_device_numbers, struct bx_diag_ctx* diag) {
    if (type_text == NULL || type_text[0] == '\0' || type_text[1] != '\0') {
        bx_diag(diag, "invalid device type '%s'", (type_text != NULL) ? type_text : "");
        return false;
    }

    switch (type_text[0]) {
        case 'b':
            *type_mode = S_IFBLK;
            *need_device_numbers = true;
            return true;
        case 'c':
        case 'u':
            *type_mode = S_IFCHR;
            *need_device_numbers = true;
            return true;
        case 'p':
            *type_mode = S_IFIFO;
            *need_device_numbers = false;
            return true;
        default:
            bx_diag(diag, "invalid device type '%s'", type_text);
            return false;
    }
}

static bool bx_mknod_parse_device_number(const char* text, const char* label, unsigned int* value, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        bx_diag(diag, "invalid %s number '%s'", label, (text != NULL) ? text : "");
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > UINT_MAX) {
        bx_diag(diag, "invalid %s number '%s'", label, text);
        return false;
    }

    *value = (unsigned int)parsed;
    return true;
}

int bx_mknod_main(int argc, char** argv) {
    struct bx_mknod_options options;
    struct bx_diag_ctx diag = {
        .progname = "mknod",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_mknod_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_mknod_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_mknod_print_version(options.progname);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag, "missing operand");
        return diag.exit_status;
    }

    if (operand_count <= 1) {
        bx_diag(&diag, "missing operand after '%s'", argv[first_operand]);
        return diag.exit_status;
    }

    const char* path = argv[first_operand];
    const char* type_text = argv[first_operand + 1];

    mode_t type_mode = 0;
    bool need_device_numbers = false;
    if (!bx_mknod_parse_type(type_text, &type_mode, &need_device_numbers, &diag)) {
        return diag.exit_status;
    }

    dev_t device = 0;
    if (need_device_numbers) {
        if (operand_count < 4) {
            bx_diag(&diag, "missing operand after '%s'", argv[first_operand + operand_count - 1]);
            return diag.exit_status;
        }
        if (operand_count > 4) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand + 4]);
            return diag.exit_status;
        }

        unsigned int major_number = 0;
        unsigned int minor_number = 0;
        if (!bx_mknod_parse_device_number(argv[first_operand + 2], "major", &major_number, &diag)) {
            return diag.exit_status;
        }
        if (!bx_mknod_parse_device_number(argv[first_operand + 3], "minor", &minor_number, &diag)) {
            return diag.exit_status;
        }

        device = makedev(major_number, minor_number);
    }
    else if (operand_count > 2) {
        bx_diag(&diag, "extra operand '%s'", argv[first_operand + 2]);
        return diag.exit_status;
    }

    if (mknod(path, type_mode | 0666u, device) != 0) {
        bx_perror_path(&diag, path);
    }

    return diag.exit_status;
}
