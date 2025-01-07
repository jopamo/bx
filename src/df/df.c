#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "applets.h"
#include "diag.h"

struct bx_df_options {
    const char* progname;
    bool show_help;
    bool show_version;
};

static const char* bx_df_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "df";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }
    return argv0;
}

static void bx_df_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "Show file system space usage for each FILE.\n");
    fprintf(stream, "With no FILE, use '.'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -k             use 1K blocks (default)\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_df_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_df_parse_options(int argc, char** argv, struct bx_df_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_df_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+k", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'k':
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

    *first_operand = optind;
    return true;
}

static uintmax_t bx_df_scale_to_1k(uintmax_t blocks, uintmax_t fragment_size) {
    if (blocks == 0u || fragment_size == 0u) {
        return 0u;
    }

    if (blocks > (UINTMAX_MAX / fragment_size)) {
        return UINTMAX_MAX;
    }

    return (blocks * fragment_size) / 1024u;
}

static unsigned bx_df_usage_percent(uintmax_t used_1k, uintmax_t available_1k) {
    uintmax_t denominator = used_1k + available_1k;
    if (denominator < used_1k) {
        denominator = UINTMAX_MAX;
    }
    if (denominator == 0u) {
        return 0u;
    }

    if (used_1k > (UINTMAX_MAX / 100u)) {
        return 100u;
    }

    uintmax_t scaled = used_1k * 100u;
    uintmax_t percent = scaled / denominator;
    if ((scaled % denominator) != 0u && percent < UINTMAX_MAX) {
        percent++;
    }
    if (percent > 100u) {
        percent = 100u;
    }

    return (unsigned)percent;
}

static bool bx_df_emit_header(struct bx_diag_ctx* diag) {
    if (fprintf(stdout, "%-20s %10s %10s %10s %4s %s\n", "Filesystem", "1K-blocks", "Used", "Available", "Use%", "Mounted on") < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

static bool bx_df_process_operand(const char* path, struct bx_diag_ctx* diag) {
    struct statvfs fs;
    if (statvfs(path, &fs) != 0) {
        bx_perror_path(diag, path);
        return true;
    }

    uintmax_t fragment_size = (fs.f_frsize != 0u) ? (uintmax_t)fs.f_frsize : (uintmax_t)fs.f_bsize;
    if (fragment_size == 0u) {
        fragment_size = 1024u;
    }

    uintmax_t total_blocks = (uintmax_t)fs.f_blocks;
    uintmax_t free_blocks = (uintmax_t)fs.f_bfree;
    uintmax_t available_blocks = (uintmax_t)fs.f_bavail;
    uintmax_t used_blocks = (total_blocks > free_blocks) ? (total_blocks - free_blocks) : 0u;

    uintmax_t total_1k = bx_df_scale_to_1k(total_blocks, fragment_size);
    uintmax_t used_1k = bx_df_scale_to_1k(used_blocks, fragment_size);
    uintmax_t available_1k = bx_df_scale_to_1k(available_blocks, fragment_size);
    unsigned usage_percent = bx_df_usage_percent(used_1k, available_1k);

    if (fprintf(stdout, "%-20s %10" PRIuMAX " %10" PRIuMAX " %10" PRIuMAX " %3u%% %s\n", path, total_1k, used_1k, available_1k, usage_percent, path) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

int bx_df_main(int argc, char** argv) {
    struct bx_df_options options;
    struct bx_diag_ctx diag = {
        .progname = "df",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_df_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_df_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_df_print_version(options.progname);
        return 0;
    }

    if (!bx_df_emit_header(&diag)) {
        return diag.exit_status;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        if (!bx_df_process_operand(".", &diag)) {
            return diag.exit_status;
        }
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            if (!bx_df_process_operand(argv[i], &diag)) {
                return diag.exit_status;
            }
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
