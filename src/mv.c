#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"
#include "common/args_common.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "common/stat_ops.h"
#include "common/backup_ops.h"
#include "common/prompt_ops.h"
#include "common/update_policy.h"

struct bx_mv_options {
    const char *progname;
    bool force;
    bool interactive;
    bool no_clobber;
    bool verbose;
    bool debug;
    bool strip_trailing_slashes;
    bool show_help;
    bool show_version;
    enum bx_update_mode update_mode;
    enum bx_backup_mode backup_mode;
    const char *suffix;
    const char *target_directory;
    bool no_target_directory;
};

static const char *bx_mv_progname(const char *argv0) {
    const char *base = strrchr(argv0, '/');
    return base ? base + 1 : argv0;
}

static void bx_mv_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "usage: %s [OPTION]... [-T] SOURCE DEST\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... SOURCE... DIRECTORY\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n", progname);
    fprintf(stream, "Rename SOURCE to DEST, or move SOURCE(s) to DIRECTORY.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Options:\n");
    fprintf(stream, "      --backup[=CONTROL]     make a backup of each existing destination file\n");
    fprintf(stream, "  -b                         like --backup but does not accept an argument\n");
    fprintf(stream, "  -f, --force                do not prompt before overwriting\n");
    fprintf(stream, "  -i, --interactive          prompt before overwrite\n");
    fprintf(stream, "  -n, --no-clobber           do not overwrite an existing file\n");
    fprintf(stream, "      --strip-trailing-slashes  remove trailing slashes from SOURCE operands\n");
    fprintf(stream, "  -S, --suffix=SUFFIX        override the usual backup suffix\n");
    fprintf(stream, "  -t, --target-directory=DIRECTORY  move all SOURCE arguments into DIRECTORY\n");
    fprintf(stream, "  -T, --no-target-directory  treat DEST as a normal file\n");
    fprintf(stream, "  -u                         same as --update=older\n");
    fprintf(stream, "      --update[=UPDATE]      control which existing files are updated;\n");
    fprintf(stream, "                               UPDATE={all,none,none-fail,older} (default: all)\n");
    fprintf(stream, "  -v, --verbose              explain what is being done\n");
    fprintf(stream, "      --debug                explain how a file is moved\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

enum {
    BX_MV_OPT_BACKUP = 256,
    BX_MV_OPT_STRIP_TRAILING_SLASHES,
    BX_MV_OPT_UPDATE,
    BX_MV_OPT_DEBUG,
};

static bool bx_mv_parse_options(int argc, char **argv, struct bx_mv_options *options, int *first_operand, struct bx_diag_ctx *diag) {
    static const struct option long_options[] = {
        {"backup", optional_argument, NULL, BX_MV_OPT_BACKUP},
        {"force", no_argument, NULL, 'f'},
        {"interactive", no_argument, NULL, 'i'},
        {"no-clobber", no_argument, NULL, 'n'},
        {"strip-trailing-slashes", no_argument, NULL, BX_MV_OPT_STRIP_TRAILING_SLASHES},
        {"suffix", required_argument, NULL, 'S'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"update", optional_argument, NULL, BX_MV_OPT_UPDATE},
        {"verbose", no_argument, NULL, 'v'},
        {"debug", no_argument, NULL, BX_MV_OPT_DEBUG},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0}
    };
    char short_opts[] = "bfint:TuvS:";

    memset(options, 0, sizeof(*options));
    options->progname = bx_mv_progname(argv[0]);
    diag->progname = options->progname;
    options->update_mode = BX_UPDATE_ALL;

    int c;
    while ((c = getopt_long(argc, argv, short_opts, long_options, NULL)) != -1) {
        switch (c) {
            case 'b':
                options->backup_mode = BX_BACKUP_UNSPECIFIED;
                break;
            case BX_MV_OPT_BACKUP:
                if (optarg) {
                    if (!bx_args_parse_backup_mode(optarg, &options->backup_mode)) {
                        bx_diag(diag, "invalid --backup control value '%s'", optarg);
                        return false;
                    }
                } else {
                    options->backup_mode = BX_BACKUP_UNSPECIFIED;
                }
                break;
            case 'f':
                options->force = true;
                options->interactive = false;
                options->no_clobber = false;
                break;
            case 'i':
                options->interactive = true;
                options->force = false;
                options->no_clobber = false;
                break;
            case 'n':
                options->no_clobber = true;
                options->force = false;
                options->interactive = false;
                break;
            case 't':
                options->target_directory = optarg;
                break;
            case 'T':
                options->no_target_directory = true;
                break;
            case 'u':
                options->update_mode = BX_UPDATE_OLDER;
                break;
            case BX_MV_OPT_UPDATE:
                if (!bx_args_parse_update_mode(optarg, &options->update_mode)) {
                    bx_diag(diag, "invalid --update mode '%s'", optarg);
                    return false;
                }
                break;
            case 'v':
                options->verbose = true;
                break;
            case BX_MV_OPT_DEBUG:
                options->debug = true;
                options->verbose = true;
                break;
            case 'S':
                options->suffix = optarg;
                break;
            case BX_MV_OPT_STRIP_TRAILING_SLASHES:
                options->strip_trailing_slashes = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

struct bx_mv_context {
    const struct bx_mv_options *options;
    struct bx_diag_ctx *diag;
    struct bx_backup_params backup_params;
};

static bool bx_mv_should_skip_existing(const struct bx_mv_options *options,
                                       const char *dest_path,
                                       const struct stat *src_stat,
                                       const struct stat *dest_stat,
                                       bool *skip_out,
                                       struct bx_diag_ctx *diag) {
    if (options->no_clobber) {
        *skip_out = true;
        return true;
    }

    if (options->interactive) {
        *skip_out = false;
        return true;
    }

    bool error = false;
    if (!bx_update_should_skip(options->update_mode, src_stat, dest_stat, skip_out, &error)) {
        if (error) {
            bx_diag(diag, "will not overwrite '%s'", dest_path);
        }
        return false;
    }
    return true;
}

static bool bx_mv_rename_file(struct bx_mv_context *ctx,
                              const char *src_path,
                              const char *dest_path) {
    struct stat src_stat;
    struct bx_dest_state dest_state;
    bool skip = false;

    if (lstat(src_path, &src_stat) != 0) {
        bx_perror_path(ctx->diag, src_path);
        return false;
    }

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(&src_stat, &dest_state.lst)) {
        /* GNU mv: error if same file, unless it's a hardlink but even then...
         * Actually GNU mv says "'src' and 'dest' are the same file"
         */
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!bx_mv_should_skip_existing(ctx->options, dest_path, &src_stat, &dest_state.lst, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive && !ctx->options->force) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        char *backup_file = bx_backup_create(dest_path, &ctx->backup_params, ctx->diag);
        if (backup_file) {
            free(backup_file);
            memset(&dest_state, 0, sizeof(dest_state));
        }
    }

    if (rename(src_path, dest_path) == 0) {
        if (ctx->options->verbose) {
            bx_info(ctx->diag, "renamed '%s' -> '%s'", src_path, dest_path);
        }
        return true;
    }

    if (errno == EXDEV) {
        /* TODO: cross-device fallback */
        bx_diag(ctx->diag, "cannot move '%s' to '%s': Cross-device link", src_path, dest_path);
        return false;
    }

    bx_perror_path(ctx->diag, dest_path);
    return false;
}

int bx_mv_main(int argc, char **argv) {
    struct bx_mv_options options;
    struct bx_diag_ctx diag = {0};
    int first_operand;
    struct bx_mv_context ctx;

    if (!bx_mv_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_mv_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        printf("bx mv version %s\n", BX_VERSION);
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count < 1) {
        bx_diag(&diag, "missing file operand");
        return 1;
    }
    if (operand_count < 2 && !options.target_directory) {
        bx_diag(&diag, "missing destination file operand after '%s'", argv[first_operand]);
        return 1;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.options = &options;
    ctx.diag = &diag;
    bx_backup_get_params(options.backup_mode, options.suffix, &ctx.backup_params);

    const char *destination_root = NULL;
    int source_count = 0;
    char **source_operands = NULL;
    bool destination_is_directory = false;

    if (options.target_directory != NULL) {
        destination_root = options.target_directory;
        source_operands = argv + first_operand;
        source_count = operand_count;
        if (!bx_stat_is_dir_path(destination_root)) {
            bx_diag(&diag, "target '%s' is not a directory", destination_root);
            return 1;
        }
        destination_is_directory = true;
    } else if (options.no_target_directory) {
        if (operand_count < 2) {
            bx_diag(&diag, "missing destination file operand after '%s'", argv[first_operand]);
            return 1;
        }
        if (operand_count > 2) {
            bx_diag(&diag, "extra operand '%s'", argv[first_operand + 2]);
            return 1;
        }
        destination_root = argv[first_operand + 1];
        source_operands = argv + first_operand;
        source_count = 1;
    } else {
        destination_root = argv[argc - 1];
        source_operands = argv + first_operand;
        source_count = operand_count - 1;

        if (source_count > 1) {
            if (!bx_stat_is_dir_path(destination_root)) {
                bx_diag(&diag, "target '%s' is not a directory", destination_root);
                return 1;
            }
            destination_is_directory = true;
        } else if (bx_stat_is_dir_path(destination_root)) {
            destination_is_directory = true;
        }
    }

    for (int i = 0; i < source_count; i++) {
        char *source_operand = options.strip_trailing_slashes
                               ? bx_path_strip_trailing_slashes_dup(source_operands[i])
                               : xstrdup(source_operands[i]);
        
        char *dest_path = bx_path_build_dest(source_operand, destination_root, destination_is_directory, false);

        bx_mv_rename_file(&ctx, source_operand, dest_path);

        free(dest_path);
        free(source_operand);
    }

    return diag.exit_status;
}
