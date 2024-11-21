#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "applets.h"
#include "common/digest_util.h"
#include "common/md5.h"

enum {
    MD5SUM_EXIT_OK = 0,
    MD5SUM_EXIT_FAIL = 1,
    MD5SUM_EXIT_USAGE = 2,
};

enum md5sum_longopt {
    MD5SUM_OPT_QUIET = 256,
    MD5SUM_OPT_STATUS,
};

struct md5sum_options {
    const char *progname;
    bool check_mode;
    bool binary_mode;
    bool quiet;
    bool status;
    bool warn;
    bool show_help;
    bool show_version;
    int first_operand;
};

static const char *md5sum_progname(const char *argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "md5sum";
}

static void md5sum_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", progname);
    fprintf(stream, "  or:  %s -c [OPTION]... [FILE]\n", progname);
    fprintf(stream, "Print or check MD5 checksums.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -b, --binary   read in binary mode\n");
    fprintf(stream, "  -c, --check    read MD5 sums from files and check them\n");
    fprintf(stream, "      --quiet    don't print OK for each successfully verified file\n");
    fprintf(stream, "      --status   don't output anything, status code shows success\n");
    fprintf(stream, "  -t, --text     read in text mode (default)\n");
    fprintf(stream, "  -w, --warn     warn about improperly formatted checksum lines\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void md5sum_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void md5_init_adapter(void *ctx) {
    bx_md5_init((struct bx_md5_ctx *)ctx);
}

static void md5_update_adapter(void *ctx, const void *data, size_t len) {
    bx_md5_update((struct bx_md5_ctx *)ctx, data, len);
}

static void md5_final_adapter(void *ctx, uint8_t *out) {
    bx_md5_final((struct bx_md5_ctx *)ctx, out);
}

static int md5sum_hash_path(const char *path, uint8_t out[BX_MD5_DIGEST_SIZE]) {
    struct bx_md5_ctx ctx;

    return bx_digest_file(&ctx,
                          sizeof(ctx),
                          md5_init_adapter,
                          md5_update_adapter,
                          md5_final_adapter,
                          path,
                          out,
                          BX_MD5_DIGEST_SIZE);
}

static int md5sum_print_hash_result(const struct md5sum_options *options, const char *path) {
    uint8_t digest[BX_MD5_DIGEST_SIZE];
    char digest_hex[BX_MD5_DIGEST_SIZE * 2u + 1u];

    if (md5sum_hash_path(path, digest) != 0) {
        fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        return MD5SUM_EXIT_FAIL;
    }

    bx_hex_encode_lower(digest, BX_MD5_DIGEST_SIZE, digest_hex);
    printf("%s %c%s\n", digest_hex, options->binary_mode ? '*' : ' ', path);
    return MD5SUM_EXIT_OK;
}

static int md5sum_verify_stream(FILE *stream,
                                const char *source_name,
                                const struct md5sum_options *options) {
    char *line = NULL;
    size_t cap = 0u;
    int exit_code = MD5SUM_EXIT_OK;
    size_t line_no = 0u;

    while (true) {
        ssize_t nread = getline(&line, &cap, stream);
        if (nread < 0) {
            break;
        }

        line_no++;
        if (nread > 0 && line[nread - 1] == '\n') {
            line[nread - 1] = '\0';
        }

        struct bx_checksum_record expected;
        if (!bx_parse_check_line(line, BX_MD5_DIGEST_SIZE, &expected)) {
            if (options->warn && !options->status) {
                fprintf(stderr,
                        "%s: %s:%zu: improperly formatted checksum line\n",
                        options->progname,
                        source_name,
                        line_no);
            }
            exit_code = MD5SUM_EXIT_FAIL;
            continue;
        }

        uint8_t actual[BX_MD5_DIGEST_SIZE];
        if (md5sum_hash_path(expected.filename, actual) != 0) {
            if (!options->status) {
                fprintf(stderr,
                        "%s: %s: %s\n",
                        options->progname,
                        expected.filename,
                        strerror(errno));
            }
            exit_code = MD5SUM_EXIT_FAIL;
            continue;
        }

        if (memcmp(actual, expected.digest, BX_MD5_DIGEST_SIZE) == 0) {
            if (!options->quiet && !options->status) {
                printf("%s: OK\n", expected.filename);
            }
            continue;
        }

        if (!options->status) {
            printf("%s: FAILED\n", expected.filename);
        }
        exit_code = MD5SUM_EXIT_FAIL;
    }

    if (ferror(stream)) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: read error\n", options->progname, source_name);
        }
        exit_code = MD5SUM_EXIT_FAIL;
    }

    free(line);
    return exit_code;
}

static int md5sum_check_file(const struct md5sum_options *options, const char *path) {
    if (strcmp(path, "-") == 0) {
        return md5sum_verify_stream(stdin, "-", options);
    }

    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        if (!options->status) {
            fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
        }
        return MD5SUM_EXIT_FAIL;
    }

    int rc = md5sum_verify_stream(stream, path, options);
    fclose(stream);
    return rc;
}

static bool md5sum_parse_options(int argc, char **argv, struct md5sum_options *options) {
    static const struct option long_options[] = {
        {"binary", no_argument, NULL, 'b'},
        {"check", no_argument, NULL, 'c'},
        {"text", no_argument, NULL, 't'},
        {"warn", no_argument, NULL, 'w'},
        {"quiet", no_argument, NULL, MD5SUM_OPT_QUIET},
        {"status", no_argument, NULL, MD5SUM_OPT_STATUS},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = md5sum_progname(argv[0]);

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+bctw", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 'b':
            options->binary_mode = true;
            break;
        case 'c':
            options->check_mode = true;
            break;
        case 't':
            options->binary_mode = false;
            break;
        case 'w':
            options->warn = true;
            break;
        case MD5SUM_OPT_QUIET:
            options->quiet = true;
            break;
        case MD5SUM_OPT_STATUS:
            options->status = true;
            break;
        case 1:
            options->show_help = true;
            break;
        case 2:
            options->show_version = true;
            break;
        case '?':
            if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                fprintf(stderr,
                        "%s: unrecognized option '%s'\n",
                        options->progname,
                        argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", options->progname);
            }
            return false;
        default:
            return false;
        }
    }

    options->first_operand = optind;
    return true;
}

int bx_md5sum_main(int argc, char **argv) {
    struct md5sum_options options;
    if (!md5sum_parse_options(argc, argv, &options)) {
        md5sum_print_help(stderr, md5sum_progname(argv[0]));
        return MD5SUM_EXIT_USAGE;
    }

    if (options.show_help) {
        md5sum_print_help(stdout, options.progname);
        return MD5SUM_EXIT_OK;
    }

    if (options.show_version) {
        md5sum_print_version(options.progname);
        return MD5SUM_EXIT_OK;
    }

    if (options.check_mode) {
        if (options.first_operand >= argc) {
            return md5sum_check_file(&options, "-");
        }

        int rc = MD5SUM_EXIT_OK;
        for (int i = options.first_operand; i < argc; i++) {
            if (md5sum_check_file(&options, argv[i]) != MD5SUM_EXIT_OK) {
                rc = MD5SUM_EXIT_FAIL;
            }
        }
        return rc;
    }

    if (options.first_operand >= argc) {
        return md5sum_print_hash_result(&options, "-");
    }

    int rc = MD5SUM_EXIT_OK;
    for (int i = options.first_operand; i < argc; i++) {
        if (md5sum_print_hash_result(&options, argv[i]) != MD5SUM_EXIT_OK) {
            rc = MD5SUM_EXIT_FAIL;
        }
    }
    return rc;
}
