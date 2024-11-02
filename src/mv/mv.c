#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/syscall.h>
#include <libgen.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"
#include "common/args_common.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "common/stat_ops.h"
#include "common/backup_ops.h"
#include "common/overwrite_ops.h"
#include "common/update_policy.h"
#include "common/copy_tree.h"
#include "common/remove_ops.h"
#include "common/copy_metadata.h"

#ifndef RENAME_EXCHANGE
# define RENAME_EXCHANGE (1 << 1)
#endif

#ifndef AT_FDCWD
# define AT_FDCWD -100
#endif

static int bx_renameat2(int oldfd, const char *oldpath, int newfd, const char *newpath, unsigned int flags) {
#ifdef SYS_renameat2
    return (int)syscall(SYS_renameat2, oldfd, oldpath, newfd, newpath, flags);
#else
    (void)oldfd; (void)oldpath; (void)newfd; (void)newpath; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

struct bx_mv_options {
    const char *progname;
    bool force;
    bool interactive;
    bool no_clobber;
    bool verbose;
    bool debug;
    bool strip_trailing_slashes;
    bool no_copy;
    bool exchange;
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

static bool bx_mv_operand_had_trailing_slashes(const char *path) {
    size_t len = strlen(path);
    return len > 1 && path[len - 1] == '/';
}

static bool bx_mv_parent_exists_as_directory(const char *path) {
    char *copy = xstrdup(path);
    char *parent = dirname(copy);
    bool is_dir = bx_stat_is_dir_path(parent);
    free(copy);
    return is_dir;
}

static bool bx_mv_should_reject_stripped_missing_dest(const struct bx_mv_options *options,
                                                      const char *src_path,
                                                      const char *dest_path,
                                                      bool source_had_trailing_slashes,
                                                      bool destination_is_directory,
                                                      int source_count) {
    struct stat src_lstat;
    struct stat src_stat;
    struct stat dest_lstat;

    if (!source_had_trailing_slashes ||
        options->no_target_directory ||
        options->target_directory != NULL ||
        destination_is_directory ||
        source_count != 1) {
        return false;
    }

    if (lstat(src_path, &src_lstat) != 0 || !S_ISLNK(src_lstat.st_mode)) {
        return false;
    }
    if (stat(src_path, &src_stat) != 0 || !S_ISDIR(src_stat.st_mode)) {
        return false;
    }
    if (lstat(dest_path, &dest_lstat) == 0) {
        return false;
    }
    if (errno != ENOENT) {
        return false;
    }

    return bx_mv_parent_exists_as_directory(dest_path);
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
    fprintf(stream, "      --no-copy              do not copy if renaming fails\n");
    fprintf(stream, "      --exchange             exchange source and destination\n");
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
    BX_MV_OPT_NO_COPY,
    BX_MV_OPT_EXCHANGE,
    BX_MV_OPT_DEBUG,
};

static bool bx_mv_parse_options(int argc, char **argv, struct bx_mv_options *options, int *first_operand, struct bx_diag_ctx *diag) {
    static const struct option long_options[] = {
        {"backup", optional_argument, NULL, BX_MV_OPT_BACKUP},
        {"force", no_argument, NULL, 'f'},
        {"interactive", no_argument, NULL, 'i'},
        {"no-clobber", no_argument, NULL, 'n'},
        {"no-copy", no_argument, NULL, BX_MV_OPT_NO_COPY},
        {"exchange", no_argument, NULL, BX_MV_OPT_EXCHANGE},
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
            case BX_MV_OPT_NO_COPY:
                options->no_copy = true;
                break;
            case BX_MV_OPT_EXCHANGE:
                options->exchange = true;
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

    if (options->target_directory != NULL && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory (-t) and --no-target-directory (-T)");
        return false;
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
    return bx_overwrite_should_skip(options->no_clobber,
                                    options->interactive,
                                    options->update_mode,
                                    dest_path,
                                    src_stat,
                                    dest_stat,
                                    skip_out,
                                    NULL,
                                    diag);
}

static bool bx_mv_directory_is_empty(const char *path,
                                     bool *empty_out,
                                     struct bx_diag_ctx *diag) {
    DIR *dir = opendir(path);
    if (dir == NULL) {
        bx_perror_path(diag, path);
        return false;
    }

    bool empty = true;
    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_perror_path(diag, path);
                ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }
        empty = false;
        break;
    }

    if (closedir(dir) != 0) {
        bx_perror_path(diag, path);
        return false;
    }

    if (!ok) {
        return false;
    }

    *empty_out = empty;
    return true;
}

static bool bx_mv_prepare_cross_device_destination(struct bx_mv_context *ctx,
                                                   const char *dest_path,
                                                   const struct stat *src_stat) {
    struct bx_dest_state dest_state;

    if (!S_ISDIR(src_stat->st_mode)) {
        return true;
    }

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    if (!dest_state.exists_lstat || !S_ISDIR(dest_state.lst.st_mode)) {
        return true;
    }

    bool empty = false;
    if (!bx_mv_directory_is_empty(dest_path, &empty, ctx->diag)) {
        return false;
    }
    if (!empty) {
        errno = ENOTEMPTY;
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (rmdir(dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (ctx->options->debug) {
        bx_info(ctx->diag,
                "cross-device move removing empty destination directory '%s' before copy",
                dest_path);
    }
    return true;
}

static bool bx_mv_cross_device_fallback(struct bx_mv_context *ctx,
                                         const char *src_path,
                                         const char *dest_path,
                                         const struct stat *src_stat) {
    (void)src_stat;
    struct bx_copy_options copy_opts = {0};
    struct bx_copy_context copy_ctx = {0};
    
    copy_opts.recursive = true;
    copy_opts.mode_policy = BX_MODE_POLICY_PRESERVE;
    copy_opts.preserve_mask = BX_PRESERVE_ALL;
    copy_opts.verbose = ctx->options->verbose;
    copy_opts.debug = ctx->options->debug;
    copy_opts.move_mode = true;
    /* Interaction was already handled in bx_mv_rename_file */
    copy_opts.force = true;
    copy_opts.interactive = false;
    copy_opts.no_clobber = false;
    copy_opts.remove_destination = true;
    copy_opts.update_mode = BX_UPDATE_ALL;

    copy_ctx.options = &copy_opts;
    copy_ctx.diag = ctx->diag;
    copy_ctx.umask_value = umask(0);
    umask(copy_ctx.umask_value);
    copy_ctx.current_source_root = src_path;
    copy_ctx.current_dest_root = dest_path;
    
    bx_backup_get_params(BX_BACKUP_NONE, NULL, &copy_ctx.backup_params);

    if (ctx->options->verbose) {
        bx_info(ctx->diag, "inter-device move: '%s' -> '%s'; copying then removing", src_path, dest_path);
    }

    if (!bx_mv_prepare_cross_device_destination(ctx, dest_path, src_stat)) {
        return false;
    }

    if (!bx_copy_path(&copy_ctx, src_path, src_path, dest_path, true)) {
        bx_copy_free_links(&copy_ctx);
        bx_copy_free_source_dirs(&copy_ctx);
        free(copy_ctx.current_dest_root_realpath);
        return false;
    }

    bx_copy_free_links(&copy_ctx);
    bx_copy_free_source_dirs(&copy_ctx);
    free(copy_ctx.current_dest_root_realpath);

    if (!bx_remove_recursive(src_path, ctx->diag)) {
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
        if (ctx->options->exchange) {
            /* No need for prompts/backups when exchanging */
        } else {
            if (!bx_mv_should_skip_existing(ctx->options, dest_path, &src_stat, &dest_state.lst, &skip, ctx->diag)) {
                return false;
            }
            if (skip) {
                return true;
            }

            if (ctx->options->interactive && !ctx->options->force) {
                if (!bx_prompt_overwrite(ctx->options->progname, dest_path)) {
                    return true;
                }
            }

            if (!bx_overwrite_backup_existing(dest_path,
                                              &ctx->backup_params,
                                              ctx->diag,
                                              &dest_state,
                                              NULL)) {
                return false;
            }
        }
    } else if (ctx->options->exchange) {
        bx_diag(ctx->diag, "cannot exchange '%s' and '%s': Destination does not exist", src_path, dest_path);
        return false;
    }

    if (ctx->options->exchange) {
        if (bx_renameat2(AT_FDCWD, src_path, AT_FDCWD, dest_path, RENAME_EXCHANGE) == 0) {
            if (ctx->options->verbose) {
                bx_info(ctx->diag, "exchanged '%s' and '%s'", src_path, dest_path);
            }
            return true;
        }
        bx_perror_path(ctx->diag, "renameat2 (exchange)");
        return false;
    }

    if (rename(src_path, dest_path) == 0) {
        if (ctx->options->verbose) {
            bx_info(ctx->diag, "renamed '%s' -> '%s'", src_path, dest_path);
        }
        return true;
    }

    if (errno == EXDEV) {
        if (ctx->options->no_copy) {
            bx_diag(ctx->diag, "cannot move '%s' to '%s': Cross-device link and --no-copy specified", src_path, dest_path);
            return false;
        }
        return bx_mv_cross_device_fallback(ctx, src_path, dest_path, &src_stat);
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

    diag.progname = options.progname;
    diag.verbose = options.verbose;
    diag.debug = options.debug;

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

    if (options.exchange) {
        if (options.target_directory || operand_count != 2) {
            bx_diag(&diag, "--exchange requires exactly two path operands and cannot be used with -t");
            return 1;
        }
        const char *src_path = argv[first_operand];
        const char *dest_path = argv[first_operand + 1];
        
        char *final_src = options.strip_trailing_slashes
                          ? bx_path_strip_trailing_slashes_dup(src_path)
                          : xstrdup(src_path);
        char *final_dst = options.strip_trailing_slashes
                          ? bx_path_strip_trailing_slashes_dup(dest_path)
                          : xstrdup(dest_path);

        bx_mv_rename_file(&ctx, final_src, final_dst);

        free(final_src);
        free(final_dst);
        return diag.exit_status;
    }

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
        bool source_had_trailing_slashes = bx_mv_operand_had_trailing_slashes(source_operands[i]);
        char *source_operand = options.strip_trailing_slashes
                               ? bx_path_strip_trailing_slashes_dup(source_operands[i])
                               : xstrdup(source_operands[i]);
        
        char *dest_path = bx_path_build_dest(source_operand, destination_root, destination_is_directory, false);

        if (bx_mv_should_reject_stripped_missing_dest(&options,
                                                      source_operand,
                                                      dest_path,
                                                      source_had_trailing_slashes,
                                                      destination_is_directory,
                                                      source_count)) {
            bx_diag(&diag, "cannot move '%s' to '%s': Not a directory", source_operand, dest_path);
            free(dest_path);
            free(source_operand);
            continue;
        }

        bx_mv_rename_file(&ctx, source_operand, dest_path);

        free(dest_path);
        free(source_operand);
    }

    return diag.exit_status;
}
