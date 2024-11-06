#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "common/copy_metadata.h"
#include "common/copy_tree.h"
#include "common/path_ops.h"
#include "common/stat_ops.h"
#include "diag.h"
#include "libbx.h"

struct bx_cp_options {
    const char *progname;
    bool strip_trailing_slashes;
    bool show_help;
    bool show_version;
    bool no_target_directory;
    enum bx_backup_mode backup_mode;
    const char *suffix;
    const char *target_directory;
    struct bx_copy_options copy;
};

enum bx_cp_longopt {
    BX_CP_OPT_ATTRIBUTES_ONLY = 256,
    BX_CP_OPT_BACKUP,
    BX_CP_OPT_COPY_CONTENTS,
    BX_CP_OPT_DEBUG,
    BX_CP_OPT_KEEP_DIRECTORY_SYMLINK,
    BX_CP_OPT_PRESERVE,
    BX_CP_OPT_NO_PRESERVE,
    BX_CP_OPT_PARENTS,
    BX_CP_OPT_REFLINK,
    BX_CP_OPT_REMOVE_DESTINATION,
    BX_CP_OPT_SPARSE,
    BX_CP_OPT_STRIP_TRAILING_SLASHES,
    BX_CP_OPT_UPDATE,
};

static const char *bx_cp_progname(const char *argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "cp";
}

static void bx_cp_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [OPTION]... [-T] SOURCE DEST\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... SOURCE... DIRECTORY\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... -t DIRECTORY SOURCE...\n", progname);
    fprintf(stream, "Copy SOURCE to DEST, or multiple SOURCE(s) to DIRECTORY.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported options:\n");
    fprintf(stream, "  -a, --archive              same as -dR --preserve=all\n");
    fprintf(stream, "      --attributes-only      don't copy file data, only create destination objects\n");
    fprintf(stream, "      --copy-contents        copy contents of special files when requested\n");
    fprintf(stream, "  -d                         same as --no-dereference --preserve=links\n");
    fprintf(stream, "  -f, --force                remove destination and retry on open/link failures\n");
    fprintf(stream, "  -H                         follow command-line symbolic links in SOURCE\n");
    fprintf(stream, "  -L, --dereference          always follow symbolic links in SOURCE\n");
    fprintf(stream, "  -P, --no-dereference       never follow symbolic links in SOURCE\n");
    fprintf(stream, "  -l, --link                 hard link files instead of copying\n");
    fprintf(stream, "  -n, --no-clobber           do not overwrite an existing destination\n");
    fprintf(stream, "  -p                         preserve mode, ownership, and timestamps\n");
    fprintf(stream, "      --preserve=ATTR_LIST   preserve mode,ownership,timestamps,links,all\n");
    fprintf(stream, "      --no-preserve=LIST     clear preserved attributes from the active set\n");
    fprintf(stream, "      --parents              use full source path under the target directory\n");
    fprintf(stream, "  -R, -r, --recursive        copy directories recursively\n");
    fprintf(stream, "      --remove-destination   unlink destination before creating/replacing it\n");
    fprintf(stream, "      --strip-trailing-slashes  remove trailing slashes from SOURCE operands\n");
    fprintf(stream, "  -s, --symbolic-link        make symbolic links instead of copying\n");
    fprintf(stream, "  -t, --target-directory=DIR copy all SOURCE arguments into DIR\n");
    fprintf(stream, "  -T, --no-target-directory  treat DEST as a normal path\n");
    fprintf(stream, "      --update[=MODE]        MODE is all, none, none-fail, or older\n");
    fprintf(stream, "  -u                         same as --update=older\n");
    fprintf(stream, "  -v, --verbose              explain what is being done\n");
    fprintf(stream, "  -x, --one-file-system      stay on this file system\n");
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
}

static void bx_cp_print_version(void) {
    printf("cp (bx) %s\n", BX_VERSION);
}

static bool bx_cp_parse_preserve_list(struct bx_diag_ctx *diag,
                                      const char *arg,
                                      unsigned *mask,
                                      bool set_bits,
                                      bool *mode_mentioned_out) {
    char *invalid_token = NULL;
    if (!bx_args_parse_preserve_list(arg, mask, set_bits, mode_mentioned_out, &invalid_token)) {
        bx_diag(diag, "invalid attribute '%s'", invalid_token);
        free(invalid_token);
        return false;
    }
    return true;
}

static bool bx_cp_parse_update_mode(struct bx_diag_ctx *diag,
                                    const char *arg,
                                    enum bx_update_mode *mode_out) {
    if (!bx_args_parse_update_mode(arg, mode_out)) {
        bx_diag(diag, "invalid --update mode '%s'", arg);
        return false;
    }
    return true;
}

static bool bx_cp_parse_options(int argc,
                                char **argv,
                                struct bx_cp_options *options,
                                int *first_operand,
                                struct bx_diag_ctx *diag) {
    static const struct option long_options[] = {
        {"archive", no_argument, NULL, 'a'},
        {"attributes-only", no_argument, NULL, BX_CP_OPT_ATTRIBUTES_ONLY},
        {"backup", optional_argument, NULL, BX_CP_OPT_BACKUP},
        {"copy-contents", no_argument, NULL, BX_CP_OPT_COPY_CONTENTS},
        {"debug", no_argument, NULL, BX_CP_OPT_DEBUG},
        {"force", no_argument, NULL, 'f'},
        {"interactive", no_argument, NULL, 'i'},
        {"dereference", no_argument, NULL, 'L'},
        {"no-dereference", no_argument, NULL, 'P'},
        {"keep-directory-symlink", no_argument, NULL, BX_CP_OPT_KEEP_DIRECTORY_SYMLINK},
        {"link", no_argument, NULL, 'l'},
        {"no-clobber", no_argument, NULL, 'n'},
        {"preserve", optional_argument, NULL, BX_CP_OPT_PRESERVE},
        {"no-preserve", required_argument, NULL, BX_CP_OPT_NO_PRESERVE},
        {"parents", no_argument, NULL, BX_CP_OPT_PARENTS},
        {"recursive", no_argument, NULL, 'R'},
        {"reflink", optional_argument, NULL, BX_CP_OPT_REFLINK},
        {"remove-destination", no_argument, NULL, BX_CP_OPT_REMOVE_DESTINATION},
        {"sparse", required_argument, NULL, BX_CP_OPT_SPARSE},
        {"strip-trailing-slashes", no_argument, NULL, BX_CP_OPT_STRIP_TRAILING_SLASHES},
        {"symbolic-link", no_argument, NULL, 's'},
        {"suffix", required_argument, NULL, 'S'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"update", optional_argument, NULL, BX_CP_OPT_UPDATE},
        {"verbose", no_argument, NULL, 'v'},
        {"one-file-system", no_argument, NULL, 'x'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };
    char short_buf[] = ":abdfHiLPlnpRrsS:t:Tuvx";

    memset(options, 0, sizeof(*options));
    options->progname = bx_cp_progname(argv[0]);
    options->copy.deref_mode = BX_DEREF_DEFAULT;
    options->copy.sparse_mode = BX_SPARSE_AUTO;
    options->copy.reflink_mode = BX_REFLINK_NEVER;
    options->copy.mode_policy = BX_MODE_POLICY_DEFAULT;
    options->copy.update_mode = BX_UPDATE_ALL;

    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, short_buf, long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
        case 1:
            options->show_help = true;
            break;
        case 2:
            options->show_version = true;
            break;
        case 'a':
            options->copy.recursive = true;
            options->copy.deref_mode = BX_DEREF_NEVER;
            options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
            options->copy.preserve_mask |= BX_PRESERVE_ALL;
            break;
        case BX_CP_OPT_ATTRIBUTES_ONLY:
            options->copy.attributes_only = true;
            break;
        case 'b':
            bx_args_enable_backup_mode(&options->backup_mode);
            break;
        case BX_CP_OPT_BACKUP:
            if (optarg == NULL) {
                bx_args_enable_backup_mode(&options->backup_mode);
            } else if (!bx_args_parse_backup_mode(optarg, &options->backup_mode)) {
                bx_diag(diag, "invalid --backup control value '%s'", optarg);
                return false;
            }
            break;
        case BX_CP_OPT_COPY_CONTENTS:
            options->copy.copy_contents = true;
            break;
        case 'S':
            options->suffix = optarg;
            break;
        case BX_CP_OPT_REFLINK:
            if (optarg == NULL) {
                options->copy.reflink_mode = BX_REFLINK_ALWAYS;
            } else if (strcmp(optarg, "always") == 0) {
                options->copy.reflink_mode = BX_REFLINK_ALWAYS;
            } else if (strcmp(optarg, "auto") == 0) {
                options->copy.reflink_mode = BX_REFLINK_AUTO;
            } else if (strcmp(optarg, "never") == 0) {
                options->copy.reflink_mode = BX_REFLINK_NEVER;
            } else {
                bx_diag(diag, "invalid --reflink argument '%s'", optarg);
                return false;
            }
            break;
        case BX_CP_OPT_SPARSE:
            if (optarg == NULL) {
                options->copy.sparse_mode = BX_SPARSE_AUTO;
            } else if (strcmp(optarg, "always") == 0) {
                options->copy.sparse_mode = BX_SPARSE_ALWAYS;
            } else if (strcmp(optarg, "auto") == 0) {
                options->copy.sparse_mode = BX_SPARSE_AUTO;
            } else if (strcmp(optarg, "never") == 0) {
                options->copy.sparse_mode = BX_SPARSE_NEVER;
            } else {
                bx_diag(diag, "invalid --sparse argument '%s'", optarg);
                return false;
            }
            break;
        case BX_CP_OPT_DEBUG:
            options->copy.debug = true;
            options->copy.verbose = true;
            break;
        case 'd':
            options->copy.deref_mode = BX_DEREF_NEVER;
            options->copy.preserve_mask |= BX_PRESERVE_LINKS;
            break;
        case 'f':
            options->copy.force = true;
            break;
        case 'i':
            options->copy.interactive = true;
            options->copy.no_clobber = false;
            break;
        case 'H':
            options->copy.deref_mode = BX_DEREF_COMMAND_LINE;
            break;
        case 'L':
            options->copy.deref_mode = BX_DEREF_ALWAYS;
            break;
        case 'P':
            options->copy.deref_mode = BX_DEREF_NEVER;
            break;
        case BX_CP_OPT_KEEP_DIRECTORY_SYMLINK:
            /* Accepted as a no-op selector for current destination handling. */
            break;
        case 'l':
            options->copy.hard_link = true;
            break;
        case 'n':
            options->copy.no_clobber = true;
            options->copy.interactive = false;
            break;
        case 'p':
            options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
            options->copy.preserve_mask |= BX_PRESERVE_MODE |
                                           BX_PRESERVE_OWNERSHIP |
                                           BX_PRESERVE_TIMESTAMPS;
            break;
        case BX_CP_OPT_PRESERVE:
            if (optarg == NULL) {
                options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                options->copy.preserve_mask |= BX_PRESERVE_MODE |
                                               BX_PRESERVE_OWNERSHIP |
                                               BX_PRESERVE_TIMESTAMPS;
            } else {
                bool mode_mentioned = false;
                if (!bx_cp_parse_preserve_list(diag,
                                               optarg,
                                               &options->copy.preserve_mask,
                                               true,
                                               &mode_mentioned)) {
                    return false;
                }
                if (mode_mentioned) {
                    options->copy.mode_policy = BX_MODE_POLICY_PRESERVE;
                }
            }
            break;
        case BX_CP_OPT_NO_PRESERVE: {
            bool mode_mentioned = false;
            if (!bx_cp_parse_preserve_list(diag,
                                           optarg,
                                           &options->copy.preserve_mask,
                                           false,
                                           &mode_mentioned)) {
                return false;
            }
            if (mode_mentioned) {
                options->copy.mode_policy = BX_MODE_POLICY_NO_PRESERVE;
            }
            break;
        }
        case BX_CP_OPT_PARENTS:
            options->copy.parents = true;
            break;
        case 'R':
        case 'r':
            options->copy.recursive = true;
            break;
        case BX_CP_OPT_REMOVE_DESTINATION:
            options->copy.remove_destination = true;
            break;
        case BX_CP_OPT_STRIP_TRAILING_SLASHES:
            options->strip_trailing_slashes = true;
            break;
        case 's':
            options->copy.symbolic_link = true;
            break;
        case 't':
            options->target_directory = optarg;
            break;
        case 'T':
            options->no_target_directory = true;
            break;
        case BX_CP_OPT_UPDATE:
            if (!bx_cp_parse_update_mode(diag, optarg, &options->copy.update_mode)) {
                return false;
            }
            break;
        case 'u':
            options->copy.update_mode = BX_UPDATE_OLDER;
            break;
        case 'v':
            options->copy.verbose = true;
            break;
        case 'x':
            options->copy.one_file_system = true;
            break;
        case ':':
            if (optopt != 0) {
                const char *arg = (optind > 0 && optind <= argc) ? argv[optind - 1] : NULL;
                if (arg != NULL && strncmp(arg, "--", 2) == 0) {
                    bx_diag(diag, "option '%s' requires an argument", arg);
                } else {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
            } else {
                bx_diag(diag, "option requires an argument");
            }
            return false;
        case '?':
            if (optopt != 0) {
                bx_diag(diag, "invalid option -- '%c'", optopt);
            } else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
            } else {
                bx_diag(diag, "unrecognized option");
            }
            return false;
        default:
            bx_diag(diag, "internal option parsing error");
            return false;
        }
    }

    if (options->copy.hard_link && options->copy.symbolic_link) {
        bx_diag(diag, "cannot combine --link and --symbolic-link");
        return false;
    }
    if (bx_args_backup_mode_requested(options->backup_mode) &&
        (options->copy.no_clobber ||
         options->copy.update_mode == BX_UPDATE_NONE ||
         options->copy.update_mode == BX_UPDATE_NONE_FAIL)) {
        bx_diag(diag, "cannot combine --backup with -n, --update=none, or --update=none-fail");
        return false;
    }
    if (options->target_directory && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory and --no-target-directory");
        return false;
    }
    if (options->copy.parents && options->no_target_directory) {
        bx_diag(diag, "--parents requires a target directory");
        return false;
    }

    *first_operand = optind;
    return true;
}

static char *bx_cp_build_dest_path(const struct bx_cp_options *options,
                                   const char *source_operand,
                                   const char *destination_root,
                                   bool destination_is_directory) {
    return bx_path_build_dest(source_operand,
                              destination_root,
                              destination_is_directory,
                              options->copy.parents);
}

static void bx_cp_init_copy_context(struct bx_copy_context *ctx,
                                    const struct bx_cp_options *options,
                                    struct bx_diag_ctx *diag,
                                    const char *destination_root,
                                    mode_t umask_value) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->options = &options->copy;
    ctx->diag = diag;
    ctx->umask_value = umask_value;
    ctx->target_root = destination_root;
    bx_backup_get_params(options->backup_mode, options->suffix, &ctx->backup_params);
}

int bx_cp_main(int argc, char **argv) {
    struct bx_cp_options options;
    int first_operand = 0;
    struct bx_copy_context copy_ctx;
    struct bx_diag_ctx diag_ctx = {0};

    if (!bx_cp_parse_options(argc, argv, &options, &first_operand, &diag_ctx)) {
        return 1;
    }

    diag_ctx.progname = options.progname;
    diag_ctx.verbose = options.copy.verbose;
    diag_ctx.debug = options.copy.debug;

    if (options.show_help) {
        bx_cp_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cp_print_version();
        return 0;
    }

    int operand_count = argc - first_operand;
    if (operand_count <= 0) {
        bx_diag(&diag_ctx, "missing file operand");
        return 1;
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
            bx_diag(&diag_ctx, "target '%s' is not a directory", destination_root);
            return 1;
        }
        destination_is_directory = true;
    } else if (options.no_target_directory) {
        if (operand_count < 2) {
            bx_diag(&diag_ctx, "missing destination file operand after '%s'", argv[first_operand]);
            return 1;
        }
        if (operand_count > 2) {
            bx_diag(&diag_ctx, "extra operand '%s'", argv[first_operand + 2]);
            return 1;
        }

        destination_root = argv[first_operand + 1];
        source_operands = argv + first_operand;
        source_count = 1;
    } else {
        if (operand_count < 2) {
            bx_diag(&diag_ctx, "missing destination file operand after '%s'", argv[first_operand]);
            return 1;
        }
        destination_root = argv[argc - 1];
        source_operands = argv + first_operand;
        source_count = operand_count - 1;

        if (source_count > 1) {
            if (!bx_stat_is_dir_path(destination_root)) {
                bx_diag(&diag_ctx, "target '%s' is not a directory", destination_root);
                return 1;
            }
            destination_is_directory = true;
        } else if (!options.no_target_directory && bx_stat_is_dir_path(destination_root)) {
            destination_is_directory = true;
        }

        if (options.copy.parents && !destination_is_directory) {
            bx_diag(&diag_ctx, "--parents requires a directory destination");
            return 1;
        }
    }

    mode_t old_umask = umask(0);
    umask(old_umask);

    bx_cp_init_copy_context(&copy_ctx, &options, &diag_ctx, destination_root, old_umask);

    for (int i = 0; i < source_count; i++) {
        char *lookup_path = xstrdup(source_operands[i]);
        char *source_operand = options.strip_trailing_slashes
                               ? bx_path_strip_trailing_slashes_dup(source_operands[i])
                               : xstrdup(source_operands[i]);
        char *dest_path = bx_cp_build_dest_path(&options, source_operand, destination_root, destination_is_directory);

        copy_ctx.stop_current_source = false;
        copy_ctx.current_source_root = lookup_path;
        copy_ctx.current_dest_root = dest_path;

        bx_copy_path(&copy_ctx, lookup_path, source_operand, dest_path, true);

        free(copy_ctx.current_dest_root_realpath);
        copy_ctx.current_dest_root_realpath = NULL;
        copy_ctx.current_dest_root = NULL;
        copy_ctx.current_source_root = NULL;
        free(dest_path);
        free(source_operand);
        free(lookup_path);
    }

    bx_copy_free_links(&copy_ctx);
    bx_copy_free_source_dirs(&copy_ctx);
    bx_copy_free_parent_attrs(&copy_ctx);
    return diag_ctx.exit_status;
}
