#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "common/copy_data.h"
#include "common/copy_metadata.h"
#include "common/update_policy.h"
#include "common/args_common.h"
#include "common/backup_ops.h"
#include "common/prompt_ops.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "common/stat_ops.h"
#include "diag.h"
#include "libbx.h"

char *realpath(const char *restrict path, char *restrict resolved_path);

enum bx_cp_deref_mode {
    BX_CP_DEREF_DEFAULT = 0,
    BX_CP_DEREF_ALWAYS,
    BX_CP_DEREF_NEVER,
    BX_CP_DEREF_COMMAND_LINE,
};

enum bx_cp_sparse_mode {
    BX_CP_SPARSE_AUTO = 0,
    BX_CP_SPARSE_ALWAYS,
    BX_CP_SPARSE_NEVER,
};

enum bx_cp_reflink_mode {
    BX_CP_REFLINK_NEVER = 0,
    BX_CP_REFLINK_AUTO,
    BX_CP_REFLINK_ALWAYS,
};

enum bx_cp_mode_policy {
    BX_CP_MODE_POLICY_DEFAULT = 0,
    BX_CP_MODE_POLICY_PRESERVE,
    BX_CP_MODE_POLICY_NO_PRESERVE,
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
    BX_CP_OPT_CONTEXT,
};

struct bx_cp_options {
    const char *progname;
    bool recursive;
    bool attributes_only;
    bool copy_contents;
    bool interactive;
    bool force;
    bool no_clobber;
    bool remove_destination;
    bool hard_link;
    bool symbolic_link;
    bool parents;
    bool no_target_directory;
    bool verbose;
    bool debug;
    bool one_file_system;
    bool strip_trailing_slashes;
    bool show_help;
    bool show_version;
    enum bx_cp_deref_mode deref_mode;
    enum bx_cp_sparse_mode sparse_mode;
    enum bx_cp_reflink_mode reflink_mode;
    enum bx_cp_mode_policy mode_policy;
    enum bx_update_mode update_mode;
    enum bx_backup_mode backup_mode;
    const char *suffix;
    unsigned preserve_mask;
    const char *target_directory;
};

struct bx_cp_link_entry {
    dev_t dev;
    ino_t ino;
    char *dest_path;
    struct bx_cp_link_entry *next;
};

struct bx_cp_dir_entry {
    dev_t dev;
    ino_t ino;
    struct bx_cp_dir_entry *next;
};

struct bx_cp_context {
    const struct bx_cp_options *options;
    struct bx_diag_ctx *diag;
    struct bx_backup_params backup_params;
    mode_t umask_value;
    struct bx_cp_link_entry *links;
    struct bx_cp_dir_entry *source_dirs;
    bool dest_root_active;
    dev_t dest_root_dev;
    ino_t dest_root_ino;
    dev_t source_root_dev;
    bool stop_current_source;
    const char *current_source_root;
    const char *current_dest_root;
    const char *target_root;
    char *current_dest_root_realpath;
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
    fprintf(stream, "      --help                 display this help and exit\n");
    fprintf(stream, "      --version              output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Not yet implemented: backup, reflink, sparse,\n");
    fprintf(stream, "one-file-system, SELinux/SMACK context handling.\n");
    fprintf(stream, "Current limitation: --copy-contents only affects FIFOs and sockets; other special files remain unsupported.\n");
}

static void bx_cp_print_version(void) {
    printf("cp (bx) %s\n", BX_VERSION);
}

static bool bx_cp_should_follow_source(const struct bx_cp_options *options,
                                       bool top_level,
                                       bool source_is_symlink) {
    if (!source_is_symlink) {
        return false;
    }

    switch (options->deref_mode) {
    case BX_CP_DEREF_ALWAYS:
        return true;
    case BX_CP_DEREF_NEVER:
        return false;
    case BX_CP_DEREF_COMMAND_LINE:
        return top_level;
    case BX_CP_DEREF_DEFAULT:
        return !options->recursive;
    }

    return false;
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
        {"sparse", optional_argument, NULL, BX_CP_OPT_SPARSE},
        {"strip-trailing-slashes", no_argument, NULL, BX_CP_OPT_STRIP_TRAILING_SLASHES},
        {"symbolic-link", no_argument, NULL, 's'},
        {"suffix", required_argument, NULL, 'S'},
        {"target-directory", required_argument, NULL, 't'},
        {"no-target-directory", no_argument, NULL, 'T'},
        {"update", optional_argument, NULL, BX_CP_OPT_UPDATE},
        {"verbose", no_argument, NULL, 'v'},
        {"one-file-system", no_argument, NULL, 'x'},
        {"context", optional_argument, NULL, BX_CP_OPT_CONTEXT},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };
    char short_buf[] = ":abdfHiLPlnpRrsS:t:TuvxZ";

    memset(options, 0, sizeof(*options));
    options->progname = bx_cp_progname(argv[0]);
    diag->progname = options->progname;
    options->deref_mode = BX_CP_DEREF_DEFAULT;
    options->mode_policy = BX_CP_MODE_POLICY_DEFAULT;
    options->update_mode = BX_UPDATE_ALL;
    options->interactive = false;
    options->force = false;
    options->no_clobber = false;
    options->remove_destination = false;

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
            options->recursive = true;
            options->deref_mode = BX_CP_DEREF_NEVER;
            options->mode_policy = BX_CP_MODE_POLICY_PRESERVE;
            options->preserve_mask |= BX_PRESERVE_ALL;
            break;
        case BX_CP_OPT_ATTRIBUTES_ONLY:
            options->attributes_only = true;
            break;
        case 'b':
            options->backup_mode = BX_BACKUP_UNSPECIFIED;
            break;
        case BX_CP_OPT_BACKUP:
            if (optarg == NULL) {
                options->backup_mode = BX_BACKUP_UNSPECIFIED;
            } else {
                if (!bx_args_parse_backup_mode(optarg, &options->backup_mode)) {
                    bx_diag(diag, "invalid --backup control value '%s'", optarg);
                    return false;
                }
            }
            break;
        case BX_CP_OPT_COPY_CONTENTS:
            options->copy_contents = true;
            break;
        case 'S':
            options->suffix = optarg;
            break;
        case BX_CP_OPT_REFLINK:
            if (optarg == NULL) {
                options->reflink_mode = BX_CP_REFLINK_ALWAYS;
            } else if (strcmp(optarg, "always") == 0) {
                options->reflink_mode = BX_CP_REFLINK_ALWAYS;
            } else if (strcmp(optarg, "auto") == 0) {
                options->reflink_mode = BX_CP_REFLINK_AUTO;
            } else if (strcmp(optarg, "never") == 0) {
                options->reflink_mode = BX_CP_REFLINK_NEVER;
            } else {
                bx_diag(diag, "invalid --reflink argument '%s'", optarg);
                return false;
            }
            break;
        case BX_CP_OPT_SPARSE:
            if (optarg == NULL) {
                options->sparse_mode = BX_CP_SPARSE_AUTO;
            } else if (strcmp(optarg, "always") == 0) {
                options->sparse_mode = BX_CP_SPARSE_ALWAYS;
            } else if (strcmp(optarg, "auto") == 0) {
                options->sparse_mode = BX_CP_SPARSE_AUTO;
            } else if (strcmp(optarg, "never") == 0) {
                options->sparse_mode = BX_CP_SPARSE_NEVER;
            } else {
                bx_diag(diag, "invalid --sparse argument '%s'", optarg);
                return false;
            }
            break;
        case BX_CP_OPT_CONTEXT:
            bx_diag(diag, "option '%s' is not implemented", argv[optind - 1]);
            return false;
        case BX_CP_OPT_DEBUG:
            options->debug = true;
            options->verbose = true;
            break;
        case 'd':
            options->deref_mode = BX_CP_DEREF_NEVER;
            options->preserve_mask |= BX_PRESERVE_LINKS;
            break;
        case 'f':
            options->force = true;
            break;
        case 'i':
            options->interactive = true;
            options->no_clobber = false;
            break;
        case 'H':
            options->deref_mode = BX_CP_DEREF_COMMAND_LINE;
            break;
        case 'L':
            options->deref_mode = BX_CP_DEREF_ALWAYS;
            break;
        case 'P':
            options->deref_mode = BX_CP_DEREF_NEVER;
            break;
        case BX_CP_OPT_KEEP_DIRECTORY_SYMLINK:
            /*
             * Current destination handling already follows existing
             * symlinks-to-directories in the covered GNU parity cases, so
             * this option is accepted as the corresponding no-op selector.
             */
            break;
        case 'l':
            options->hard_link = true;
            break;
        case 'n':
            options->no_clobber = true;
            options->interactive = false;
            break;
        case 'p':
            options->mode_policy = BX_CP_MODE_POLICY_PRESERVE;
            options->preserve_mask |= BX_PRESERVE_MODE |
                                      BX_PRESERVE_OWNERSHIP |
                                      BX_PRESERVE_TIMESTAMPS;
            break;
        case BX_CP_OPT_PRESERVE:
            if (optarg == NULL) {
                options->mode_policy = BX_CP_MODE_POLICY_PRESERVE;
                options->preserve_mask |= BX_PRESERVE_MODE |
                                          BX_PRESERVE_OWNERSHIP |
                                          BX_PRESERVE_TIMESTAMPS;
            } else {
                bool mode_mentioned = false;
                if (!bx_cp_parse_preserve_list(diag,
                                               optarg,
                                               &options->preserve_mask,
                                               true,
                                               &mode_mentioned)) {
                    return false;
                }
                if (mode_mentioned) {
                    options->mode_policy = BX_CP_MODE_POLICY_PRESERVE;
                }
            }
            break;
        case BX_CP_OPT_NO_PRESERVE: {
            bool mode_mentioned = false;
            if (!bx_cp_parse_preserve_list(diag,
                                           optarg,
                                           &options->preserve_mask,
                                           false,
                                           &mode_mentioned)) {
                return false;
            }
            if (mode_mentioned) {
                options->mode_policy = BX_CP_MODE_POLICY_NO_PRESERVE;
            }
            break;
        }
        case BX_CP_OPT_PARENTS:
            options->parents = true;
            break;
        case 'R':
        case 'r':
            options->recursive = true;
            break;
        case BX_CP_OPT_REMOVE_DESTINATION:
            options->remove_destination = true;
            break;
        case BX_CP_OPT_STRIP_TRAILING_SLASHES:
            options->strip_trailing_slashes = true;
            break;
        case 's':
            options->symbolic_link = true;
            break;
        case 't':
            options->target_directory = optarg;
            break;
        case 'T':
            options->no_target_directory = true;
            break;
        case BX_CP_OPT_UPDATE:
            if (!bx_cp_parse_update_mode(diag, optarg, &options->update_mode)) {
                return false;
            }
            break;
        case 'u':
            options->update_mode = BX_UPDATE_OLDER;
            break;
        case 'v':
            options->verbose = true;
            break;
        case 'x':
            options->one_file_system = true;
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

    if (options->hard_link && options->symbolic_link) {
        bx_diag(diag, "cannot combine --link and --symbolic-link");
        return false;
    }
    if (options->target_directory && options->no_target_directory) {
        bx_diag(diag, "cannot combine --target-directory and --no-target-directory");
        return false;
    }
    if (options->parents && options->no_target_directory) {
        bx_diag(diag, "--parents requires a target directory");
        return false;
    }

    *first_operand = optind;
    return true;
}

static struct bx_cp_link_entry *bx_cp_find_link_entry(struct bx_cp_context *ctx,
                                                      dev_t dev,
                                                      ino_t ino) {
    for (struct bx_cp_link_entry *entry = ctx->links; entry != NULL; entry = entry->next) {
        if (entry->dev == dev && entry->ino == ino) {
            return entry;
        }
    }
    return NULL;
}

static void bx_cp_add_link_entry(struct bx_cp_context *ctx,
                                 const struct stat *st,
                                 const char *dest_path) {
    struct bx_cp_link_entry *entry;

    if (st->st_nlink < 2) {
        return;
    }
    if (bx_cp_find_link_entry(ctx, st->st_dev, st->st_ino) != NULL) {
        return;
    }

    entry = xmalloc(sizeof(*entry));
    entry->dev = st->st_dev;
    entry->ino = st->st_ino;
    entry->dest_path = xstrdup(dest_path);
    entry->next = ctx->links;
    ctx->links = entry;
}

static void bx_cp_free_links(struct bx_cp_context *ctx) {
    struct bx_cp_link_entry *entry = ctx->links;
    while (entry != NULL) {
        struct bx_cp_link_entry *next = entry->next;
        free(entry->dest_path);
        free(entry);
        entry = next;
    }
    ctx->links = NULL;
}

static bool bx_cp_source_dir_in_stack(const struct bx_cp_context *ctx,
                                      dev_t dev,
                                      ino_t ino) {
    for (struct bx_cp_dir_entry *entry = ctx->source_dirs; entry != NULL; entry = entry->next) {
        if (entry->dev == dev && entry->ino == ino) {
            return true;
        }
    }
    return false;
}

static void bx_cp_push_source_dir(struct bx_cp_context *ctx, const struct stat *st) {
    struct bx_cp_dir_entry *entry = xmalloc(sizeof(*entry));
    entry->dev = st->st_dev;
    entry->ino = st->st_ino;
    entry->next = ctx->source_dirs;
    ctx->source_dirs = entry;
}

static void bx_cp_pop_source_dir(struct bx_cp_context *ctx) {
    struct bx_cp_dir_entry *entry = ctx->source_dirs;
    if (entry == NULL) {
        return;
    }
    ctx->source_dirs = entry->next;
    free(entry);
}

static void bx_cp_free_source_dirs(struct bx_cp_context *ctx) {
    while (ctx->source_dirs != NULL) {
        bx_cp_pop_source_dir(ctx);
    }
}

static bool bx_cp_should_skip_existing(const struct bx_cp_options *options,
                                       const char *dest_path,
                                       const struct stat *src_stat,
                                       const struct stat *dest_stat,
                                       bool *skip_out,
                                       struct bx_diag_ctx *diag) {
    if (options->no_clobber) {
        bx_debug(diag, "skipping '%s' because of -n", dest_path);
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
    if (*skip_out) {
        bx_debug(diag, "skipping '%s' because of --update", dest_path);
    }
    return true;
}

static bool bx_cp_prepare_parents(struct bx_cp_context *ctx, const char *source_operand) {
    char *src_copy = xstrdup(source_operand);
    size_t len = strlen(src_copy);
    size_t start = 0;

    if (len == 0) {
        free(src_copy);
        return true;
    }
    if (src_copy[0] == '/') {
        start = 1;
    }

    for (size_t i = start; src_copy[i] != '\0'; i++) {
        if (src_copy[i] != '/') {
            continue;
        }
        src_copy[i] = '\0';
        if (src_copy[0] != '\0' || start == 1) {
            char *current_src = (start == 1 && src_copy[0] == '\0') ? "/" : src_copy;
            char *current_dest = bx_path_join(ctx->target_root, current_src);
            struct stat src_st;

            if (stat(current_src, &src_st) != 0) {
                bx_perror_path(ctx->diag, current_src);
                free(current_dest);
                free(src_copy);
                return false;
            }

            if (mkdir(current_dest, 0777u & ~ctx->umask_value) != 0) {
                if (errno != EEXIST) {
                    bx_perror_path(ctx->diag, current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
                struct stat dest_st;
                if (stat(current_dest, &dest_st) != 0) {
                    bx_perror_path(ctx->diag, current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
                if (!S_ISDIR(dest_st.st_mode)) {
                    bx_diag(ctx->diag, "cannot create directory '%s': Not a directory", current_dest);
                    free(current_dest);
                    free(src_copy);
                    return false;
                }
            }

            if (ctx->options->mode_policy == BX_CP_MODE_POLICY_PRESERVE ||
                (ctx->options->preserve_mask & (BX_PRESERVE_OWNERSHIP | BX_PRESERVE_TIMESTAMPS | BX_PRESERVE_XATTR | BX_PRESERVE_CONTEXT)) != 0u) {
                if (!bx_copy_path_metadata(current_src, current_dest, &src_st, ctx->options->preserve_mask, false)) {
                    /* GNU cp --parents -p: if it fails to preserve metadata, it's a warning but continues?
                     * Actually it seems it's a non-fatal error but sets exit status.
                     * For now we follow the same pattern as other metadata copies.
                     */
                }
            }
            free(current_dest);
        }
        src_copy[i] = '/';
    }

    free(src_copy);
    return true;
}

static bool bx_cp_apply_fd_attrs(const struct bx_cp_context *ctx,
                                 int src_fd,
                                 int dest_fd,
                                 const struct stat *src_stat) {
    if (!bx_copy_fd_metadata(src_fd, dest_fd, src_stat, ctx->options->preserve_mask)) {
        bx_perror_path(ctx->diag, "fchown/fchmod/futimens/fsetxattr");
        return false;
    }
    return true;
}

static bool bx_cp_apply_path_attrs(const struct bx_cp_context *ctx,
                                   const char *src_path,
                                   const char *dest_path,
                                   const struct stat *src_stat,
                                   bool no_follow,
                                   bool is_directory) {
    (void)is_directory;
    if (!bx_copy_path_metadata(src_path, dest_path, src_stat, ctx->options->preserve_mask, no_follow)) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static mode_t bx_cp_directory_create_mode(const struct bx_cp_context *ctx,
                                          const struct stat *src_stat,
                                          mode_t *final_mode_out,
                                          bool *restore_mode_out) {
    mode_t source_mode = src_stat->st_mode & 0777u;

    if (ctx->options->mode_policy == BX_CP_MODE_POLICY_PRESERVE) {
        *final_mode_out = 0;
        *restore_mode_out = false;
        return source_mode | S_IRWXU;
    }

    if (ctx->options->mode_policy == BX_CP_MODE_POLICY_NO_PRESERVE) {
        *final_mode_out = 0777u & ~ctx->umask_value;
    } else {
        *final_mode_out = source_mode & ~ctx->umask_value;
    }
    *restore_mode_out = (*final_mode_out | S_IRWXU) != *final_mode_out;
    return *final_mode_out | S_IRWXU;
}

static mode_t bx_cp_regular_file_create_mode(const struct bx_cp_context *ctx,
                                             const struct stat *src_stat) {
    if (ctx->options->mode_policy == BX_CP_MODE_POLICY_NO_PRESERVE) {
        return 0666;
    }
    return src_stat->st_mode & 0777u;
}

static void bx_cp_print_verbose(const struct bx_cp_context *ctx,
                                 const char *src_path,
                                 const char *dest_path) {
    bx_info(ctx->diag, "'%s' -> '%s'", src_path, dest_path);
}
static void bx_cp_diag_self_recursive_copy(struct bx_cp_context *ctx,
                                           const char *src_path,
                                           const char *dest_path) {
    const char *diag_src = ctx->current_source_root ? ctx->current_source_root : src_path;
    const char *diag_dest = ctx->current_dest_root ? ctx->current_dest_root : dest_path;

    bx_diag(ctx->diag,
               "cannot copy a directory, '%s', into itself, '%s'",
               diag_src,
               diag_dest);
    ctx->stop_current_source = true;
}

static void bx_cp_diag_cyclic_symlink(struct bx_cp_context *ctx, const char *src_path) {
    bx_diag(ctx->diag, "cannot copy cyclic symbolic link '%s'", src_path);
}

static char *bx_cp_realpath_dup(const char *path) {
    char resolved[PATH_MAX];

    if (realpath(path, resolved) == NULL) {
        return NULL;
    }
    return xstrdup(resolved);
}

static char *bx_cp_required_self_copy_child(const struct bx_cp_context *ctx,
                                            const char *src_path) {
    if (ctx->current_dest_root_realpath == NULL) {
        return NULL;
    }

    char *src_realpath = bx_cp_realpath_dup(src_path);
    if (src_realpath == NULL) {
        return NULL;
    }

    size_t src_len = strlen(src_realpath);
    const char *dest_realpath = ctx->current_dest_root_realpath;
    if (strncmp(dest_realpath, src_realpath, src_len) != 0 || dest_realpath[src_len] != '/') {
        free(src_realpath);
        return NULL;
    }

    const char *suffix = dest_realpath + src_len + 1;
    if (strchr(suffix, '/') == NULL) {
        free(src_realpath);
        return NULL;
    }
    size_t component_len = strcspn(suffix, "/");
    char *component = xmalloc(component_len + 1u);

    memcpy(component, suffix, component_len);
    component[component_len] = '\0';
    free(src_realpath);
    return component;
}

static bool bx_cp_copy_data(int src_fd, int dest_fd, struct bx_diag_ctx *diag, const struct bx_cp_options *options) {
    struct bx_copy_data_options data_opts;
    data_opts.sparse_mode = (enum bx_sparse_mode)options->sparse_mode;
    data_opts.reflink_mode = (enum bx_reflink_mode)options->reflink_mode;

    int res = bx_copy_data(src_fd, dest_fd, &data_opts);
    if (res == BX_COPY_DATA_SUCCESS) {
        return true;
    }
    if (res == BX_COPY_DATA_READ_ERROR) {
        bx_perror_path(diag, "read");
    } else if (res == BX_COPY_DATA_WRITE_ERROR) {
        bx_perror_path(diag, "write/lseek/ftruncate");
    } else if (res == BX_COPY_DATA_REFLINK_FAILED) {
        bx_diag(diag, "failed to clone '%s'", "destination"); // We don't have the path here easily
    }
    return false;
}

static bool bx_cp_unlink_existing_file(const struct bx_cp_context *ctx, const char *dest_path) {
    if (unlink(dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_cp_reject_directory_dest(const struct bx_cp_context *ctx,
                                        const char *source_path,
                                        const char *dest_path,
                                        const struct bx_dest_state *dest_state) {
    if (!dest_state->exists_lstat || !S_ISDIR(dest_state->lst.st_mode)) {
        return true;
    }

    bx_diag(ctx->diag, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, source_path);
    return false;
}

static bool bx_cp_backup_existing_dest(struct bx_cp_context *ctx,
                                       const char *dest_path,
                                       struct bx_dest_state *dest_state) {
    char *backup_file = NULL;
    enum bx_backup_create_result result =
        bx_backup_create(dest_path, &ctx->backup_params, ctx->diag, &backup_file);

    if (result == BX_BACKUP_CREATE_FAILED) {
        return false;
    }
    if (result == BX_BACKUP_CREATE_CREATED) {
        free(backup_file);
        memset(dest_state, 0, sizeof(*dest_state));
    }
    return true;
}

static enum bx_backup_create_result bx_cp_backup_same_file_copy(struct bx_cp_context *ctx,
                                                                const char *src_path,
                                                                const char *dest_path) {
    char *backup_file = NULL;
    enum bx_backup_create_result result =
        bx_backup_create_copy(dest_path, &ctx->backup_params, ctx->diag, &backup_file);

    if (result == BX_BACKUP_CREATE_CREATED) {
        bx_info(ctx->diag, "'%s' -> '%s'", src_path, backup_file);
        free(backup_file);
    }
    return result;
}

static bool bx_cp_copy_regular_file(struct bx_cp_context *ctx,
                                    const char *src_path,
                                    const char *dest_path,
                                    const struct stat *src_stat,
                                    bool open_source_for_attributes_only) {
    struct bx_dest_state dest_state;
    bool skip = false;
    int src_fd = -1;
    int dest_fd = -1;
    mode_t create_mode = bx_cp_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        if (ctx->backup_params.mode != BX_BACKUP_NONE && ctx->options->force &&
            S_ISREG(src_stat->st_mode) && strcmp(src_path, dest_path) == 0) {
            enum bx_backup_create_result backup_result =
                bx_cp_backup_same_file_copy(ctx, src_path, dest_path);
            if (backup_result == BX_BACKUP_CREATE_CREATED) {
                return true;
            }
            if (backup_result == BX_BACKUP_CREATE_FAILED) {
                return false;
            }
        }
        bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.dangling_symlink && !ctx->options->remove_destination) {
        bx_diag(ctx->diag, "not writing through dangling symlink '%s'", dest_path);
        return false;
    }

    if (dest_state.exists_stat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        if (!bx_cp_reject_directory_dest(ctx, src_path, dest_path, &dest_state)) {
            return false;
        }

        if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
            return false;
        }
    }

    if (!ctx->options->attributes_only || open_source_for_attributes_only ||
        (ctx->options->preserve_mask & (BX_PRESERVE_XATTR | BX_PRESERVE_CONTEXT | BX_PRESERVE_MODE)) != 0u) {
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) {
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
    }

    if (ctx->options->remove_destination && dest_state.exists_lstat) {
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            goto fail;
        }
        memset(&dest_state, 0, sizeof(dest_state));
    }

    int dest_open_flags = O_WRONLY | O_CREAT;
    if (!ctx->options->attributes_only) {
        dest_open_flags |= O_TRUNC;
    }

    dest_fd = open(dest_path, dest_open_flags, create_mode);
    if (dest_fd < 0 && ctx->options->force && dest_state.exists_lstat) {
        if (bx_cp_unlink_existing_file(ctx, dest_path)) {
            dest_fd = open(dest_path, dest_open_flags, create_mode);
        }
    }
    if (dest_fd < 0) {
        if (dest_state.dangling_symlink && !ctx->options->force && !ctx->options->remove_destination) {
            bx_diag(ctx->diag, "not writing through dangling symlink '%s'", dest_path);
        } else {
            bx_perror_path(ctx->diag, dest_path);
        }
        goto fail;
    }

    if (!ctx->options->attributes_only && !bx_cp_copy_data(src_fd, dest_fd, ctx->diag, ctx->options)) {
        goto fail;
    }
    if (!bx_cp_apply_fd_attrs(ctx, src_fd, dest_fd, src_stat)) {
        goto fail;
    }

    if (close(dest_fd) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        dest_fd = -1;
        goto fail;
    }
    dest_fd = -1;

    if (src_fd >= 0 && close(src_fd) != 0) {
        bx_perror_path(ctx->diag, src_path);
        src_fd = -1;
        return false;
    }
    src_fd = -1;

    bx_cp_add_link_entry(ctx, src_stat, dest_path);
    bx_cp_print_verbose(ctx, src_path, dest_path);
    return true;

fail:
    if (dest_fd >= 0) {
        close(dest_fd);
    }
    if (src_fd >= 0) {
        close(src_fd);
    }
    return false;
}

static bool bx_cp_copy_regular_file_path(struct bx_cp_context *ctx,
                                         const char *src_path,
                                         const char *dest_path,
                                         const struct stat *src_stat) {
    return bx_cp_copy_regular_file(ctx, src_path, dest_path, src_stat, false);
}

static bool bx_cp_copy_fifo_contents(struct bx_cp_context *ctx,
                                     const char *src_path,
                                     const char *dest_path,
                                     const struct stat *src_stat) {
    return bx_cp_copy_regular_file(ctx, src_path, dest_path, src_stat, true);
}

static bool bx_cp_create_fifo_node(struct bx_cp_context *ctx,
                                   const char *dest_path,
                                   mode_t create_mode) {
    if (mkfifo(dest_path, create_mode) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_cp_create_socket_node(struct bx_cp_context *ctx,
                                     const char *dest_path,
                                     mode_t create_mode) {
    size_t path_len = strlen(dest_path);
    struct sockaddr_un addr;
    int fd;

    if (path_len >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, dest_path, path_len + 1u);

    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
    if (bind(fd, (const struct sockaddr *)&addr, addr_len) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        close(fd);
        return false;
    }

    if (chmod(dest_path, create_mode) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        close(fd);
        return false;
    }

    if (close(fd) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    return true;
}

static bool bx_cp_copy_special_node(struct bx_cp_context *ctx,
                                    const char *src_path,
                                    const char *dest_path,
                                    const struct stat *src_stat,
                                    bool (*create_node)(struct bx_cp_context *ctx,
                                                        const char *dest_path,
                                                        mode_t create_mode)) {
    struct bx_dest_state dest_state;
    bool skip = false;
    mode_t create_mode = bx_cp_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_stat, &dest_state.lst)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.lst, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        if (!bx_cp_reject_directory_dest(ctx, src_path, dest_path, &dest_state)) {
            return false;
        }

        if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
            return false;
        }

        if (dest_state.exists_lstat) {
            if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
                return false;
            }
        }
    }

    if (!create_node(ctx, dest_path, create_mode)) {
        return false;
    }

    if (!bx_cp_apply_path_attrs(ctx, src_path, dest_path, src_stat, false, false)) {
        return false;
    }

    bx_cp_add_link_entry(ctx, src_stat, dest_path);
    bx_cp_print_verbose(ctx, src_path, dest_path);
    return true;
}

static bool bx_cp_create_device_node(struct bx_cp_context *ctx,
                                     const char *dest_path,
                                     mode_t create_mode,
                                     dev_t rdev) {
    if (mknod(dest_path, create_mode, rdev) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }
    return true;
}

static bool bx_cp_copy_device_node(struct bx_cp_context *ctx,
                                   const char *src_path,
                                   const char *dest_path,
                                   const struct stat *src_stat) {
    struct bx_dest_state dest_state;
    bool skip = false;
    mode_t create_mode = bx_cp_regular_file_create_mode(ctx, src_stat);

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_stat, &dest_state.lst)) {
        bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.lst, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        if (!bx_cp_reject_directory_dest(ctx, src_path, dest_path, &dest_state)) {
            return false;
        }

        if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
            return false;
        }

        if (dest_state.exists_lstat) {
            if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
                return false;
            }
        }
    }

    if (!bx_cp_create_device_node(ctx, dest_path, create_mode, src_stat->st_rdev)) {
        return false;
    }

    if (!bx_cp_apply_path_attrs(ctx, src_path, dest_path, src_stat, false, false)) {
        return false;
    }

    bx_cp_add_link_entry(ctx, src_stat, dest_path);
    bx_cp_print_verbose(ctx, src_path, dest_path);
    return true;
}

static bool bx_cp_copy_fifo(struct bx_cp_context *ctx,
                            const char *src_path,
                            const char *dest_path,
                            const struct stat *src_stat) {
    return bx_cp_copy_special_node(ctx, src_path, dest_path, src_stat, bx_cp_create_fifo_node);
}

static bool bx_cp_copy_socket_contents(struct bx_cp_context *ctx,
                                       const char *src_path,
                                       const char *dest_path,
                                       const struct stat *src_stat) {
    return bx_cp_copy_regular_file(ctx, src_path, dest_path, src_stat, true);
}

static bool bx_cp_copy_socket(struct bx_cp_context *ctx,
                              const char *src_path,
                              const char *dest_path,
                              const struct stat *src_stat) {
    return bx_cp_copy_special_node(ctx, src_path, dest_path, src_stat, bx_cp_create_socket_node);
}

static bool bx_cp_copy_symlink_object(struct bx_cp_context *ctx,
                                      const char *src_path,
                                      const char *dest_path,
                                      const struct stat *src_lstat) {
    struct bx_dest_state dest_state;
    bool skip = false;
    ssize_t target_size = src_lstat->st_size > 0 ? src_lstat->st_size : 256;
    char *link_target = NULL;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_lstat, &dest_state.lst)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_lstat, &dest_state.lst, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        if (!bx_cp_reject_directory_dest(ctx, src_path, dest_path, &dest_state)) {
            return false;
        }

        if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
            return false;
        }

        if (dest_state.exists_lstat) {
            if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
                return false;
            }
        }
    }

    while (true) {
        link_target = xrealloc(link_target, (size_t)target_size + 1u);
        ssize_t nread = readlink(src_path, link_target, (size_t)target_size);
        if (nread < 0) {
            free(link_target);
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
        if (nread < target_size) {
            link_target[nread] = '\0';
            break;
        }
        target_size *= 2;
    }

    if (symlink(link_target, dest_path) != 0) {
        free(link_target);
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (!bx_cp_apply_path_attrs(ctx, src_path, dest_path, src_lstat, true, false)) {
        free(link_target);
        return false;
    }

    bx_cp_add_link_entry(ctx, src_lstat, dest_path);
    bx_cp_print_verbose(ctx, src_path, dest_path);
    free(link_target);
    return true;
}

static bool bx_cp_create_symbolic_link(struct bx_cp_context *ctx,
                                       const char *source_operand,
                                       const char *dest_path,
                                       const struct stat *src_stat) {
    struct bx_dest_state dest_state;
    bool skip = false;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", source_operand, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (dest_state.exists_stat) {
            if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip, ctx->diag)) {
                return false;
            }
            if (skip) {
                return true;
            }

            if (ctx->options->interactive) {
                char prompt[PATH_MAX + 32];
                snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
                if (!bx_prompt_confirm(prompt)) {
                    return true;
                }
            }

            if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
                return false;
            }
        }
        if (dest_state.exists_lstat) {
            if (S_ISDIR(dest_state.lst.st_mode)) {
                bx_diag(ctx->diag, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, source_operand);
                return false;
            }
            if (!(ctx->options->force || ctx->options->remove_destination)) {
                errno = EEXIST;
                bx_perror_path(ctx->diag, dest_path);
                return false;
            }
            if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
                return false;
            }
        }
    }

    if (symlink(source_operand, dest_path) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    bx_cp_print_verbose(ctx, source_operand, dest_path);
    return true;
}

static bool bx_cp_create_hard_link(struct bx_cp_context *ctx,
                                   const char *src_path,
                                   const char *dest_path,
                                   const struct stat *src_stat,
                                   bool follow_source) {
    struct bx_dest_state dest_state;
    bool skip = false;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_debug(ctx->diag, "skipping '%s' because it is the same file as '%s'", src_path, dest_path);
        bx_diag(ctx->diag, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_stat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip, ctx->diag)) {
            return false;
        }
        if (skip) {
            return true;
        }

        if (ctx->options->interactive) {
            char prompt[PATH_MAX + 32];
            snprintf(prompt, sizeof(prompt), "%s: overwrite '%s'? ", ctx->options->progname, dest_path);
            if (!bx_prompt_confirm(prompt)) {
                return true;
            }
        }

        if (!bx_cp_backup_existing_dest(ctx, dest_path, &dest_state)) {
            return false;
        }
    }

    if (dest_state.exists_lstat) {
        if (S_ISDIR(dest_state.lst.st_mode)) {
            bx_diag(ctx->diag, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, src_path);
            return false;
        }
        if (!(ctx->options->force || ctx->options->remove_destination)) {
            errno = EEXIST;
            bx_perror_path(ctx->diag, dest_path);
            return false;
        }
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            return false;
        }
    }

    if (linkat(AT_FDCWD,
               src_path,
               AT_FDCWD,
               dest_path,
               follow_source ? AT_SYMLINK_FOLLOW : 0) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    bx_cp_print_verbose(ctx, src_path, dest_path);
    return true;
}

static bool bx_cp_copy_path(struct bx_cp_context *ctx,
                            const char *src_path,
                            const char *source_operand,
                            const char *dest_path,
                            bool top_level);

static bool bx_cp_copy_directory(struct bx_cp_context *ctx,
                                 const char *src_path,
                                 const char *dest_path,
                                 const struct stat *src_stat,
                                 bool top_level) {
    struct bx_dest_state dest_state;
    bool created = false;
    bool restore_mode = false;
    mode_t final_mode = 0;
    bool prev_dest_root_active = ctx->dest_root_active;
    dev_t prev_dest_root_dev = ctx->dest_root_dev;
    ino_t prev_dest_root_ino = ctx->dest_root_ino;
    DIR *dir = NULL;
    bool ok = true;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_perror_path(ctx->diag, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!S_ISDIR(dest_state.lst.st_mode)) {
            bx_diag(ctx->diag, "cannot overwrite non-directory '%s' with directory '%s'", dest_path, src_path);
            return false;
        }
    } else {
        mode_t mkdir_mode = bx_cp_directory_create_mode(ctx, src_stat, &final_mode, &restore_mode);
        if (mkdir(dest_path, mkdir_mode) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            return false;
        }
        created = true;
        if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            return false;
        }
    }

    if (top_level) {
        ctx->dest_root_active = true;
        ctx->dest_root_dev = dest_state.lst.st_dev;
        ctx->dest_root_ino = dest_state.lst.st_ino;
        free(ctx->current_dest_root_realpath);
        ctx->current_dest_root_realpath = bx_cp_realpath_dup(dest_path);
    }

    bx_cp_push_source_dir(ctx, src_stat);

    dir = opendir(src_path);
    if (dir == NULL) {
        bx_perror_path(ctx->diag, src_path);
        ok = false;
        goto finish;
    }

    char *required_child = bx_cp_required_self_copy_child(ctx, src_path);
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_perror_path(ctx->diag, src_path);
                ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }
        if (required_child != NULL && strcmp(entry->d_name, required_child) != 0) {
            continue;
        }

        char *src_child = bx_path_join(src_path, entry->d_name);
        struct stat child_lstat;
        if (lstat(src_child, &child_lstat) != 0) {
            bx_perror_path(ctx->diag, src_child);
            free(src_child);
            ok = false;
            continue;
        }
        if (ctx->dest_root_active &&
            S_ISDIR(child_lstat.st_mode) &&
            child_lstat.st_dev == ctx->dest_root_dev &&
            child_lstat.st_ino == ctx->dest_root_ino) {
            bx_cp_diag_self_recursive_copy(ctx, src_path, dest_path);
            free(src_child);
            ok = false;
            continue;
        }

        char *dest_child = bx_path_join(dest_path, entry->d_name);
        if (!bx_cp_copy_path(ctx, src_child, src_child, dest_child, false)) {
            ok = false;
            free(dest_child);
            free(src_child);
            if (ctx->stop_current_source) {
                break;
            }
            continue;
        }
        free(dest_child);
        free(src_child);
    }
    free(required_child);

    if (closedir(dir) != 0) {
        bx_perror_path(ctx->diag, src_path);
        ok = false;
    }
    dir = NULL;

finish:
    if (created && restore_mode) {
        if (chmod(dest_path, final_mode) != 0) {
            bx_perror_path(ctx->diag, dest_path);
            ok = false;
        }
    }
    if (!bx_cp_apply_path_attrs(ctx, src_path, dest_path, src_stat, false, true)) {
        return false;
    }
    if (ok && created) {
        bx_cp_print_verbose(ctx, src_path, dest_path);
    }

    bx_cp_pop_source_dir(ctx);
    ctx->dest_root_active = prev_dest_root_active;
    ctx->dest_root_dev = prev_dest_root_dev;
    ctx->dest_root_ino = prev_dest_root_ino;
    return ok;
}

static bool bx_cp_copy_path(struct bx_cp_context *ctx,
                            const char *src_path,
                            const char *source_operand,
                            const char *dest_path,
                            bool top_level) {
    struct stat src_lstat;
    struct stat src_stat;
    bool source_is_symlink;
    bool follow_source;

    if (lstat(src_path, &src_lstat) != 0) {
        bx_perror_path(ctx->diag, src_path);
        return false;
    }

    if (!top_level && ctx->options->one_file_system && src_lstat.st_dev != ctx->source_root_dev) {
        return true;
    }

    source_is_symlink = S_ISLNK(src_lstat.st_mode);
    follow_source = bx_cp_should_follow_source(ctx->options, top_level, source_is_symlink);

    if (follow_source) {
        if (stat(src_path, &src_stat) != 0) {
            bx_perror_path(ctx->diag, src_path);
            return false;
        }
    } else {
        src_stat = src_lstat;
    }

    if (top_level) {
        ctx->source_root_dev = src_stat.st_dev;
    }

    if (follow_source &&
        source_is_symlink &&
        S_ISDIR(src_stat.st_mode) &&
        bx_cp_source_dir_in_stack(ctx, src_stat.st_dev, src_stat.st_ino)) {
        bx_cp_diag_cyclic_symlink(ctx, src_path);
        return false;
    }

    if (ctx->options->parents && !bx_cp_prepare_parents(ctx, source_operand)) {
        return false;
    }

    if (S_ISDIR(src_stat.st_mode)) {
        if (!ctx->options->recursive) {
            bx_diag(ctx->diag, "-r not specified; omitting directory '%s'", src_path);
            return false;
        }
        return bx_cp_copy_directory(ctx, src_path, dest_path, &src_stat, top_level);
    }

    if (ctx->options->symbolic_link) {
        return bx_cp_create_symbolic_link(ctx, source_operand, dest_path, &src_stat);
    }
    if (ctx->options->hard_link) {
        return bx_cp_create_hard_link(ctx, src_path, dest_path, &src_stat, follow_source);
    }

    if ((ctx->options->preserve_mask & BX_PRESERVE_LINKS) != 0u &&
        !ctx->options->hard_link &&
        !ctx->options->symbolic_link) {
        struct bx_cp_link_entry *entry = bx_cp_find_link_entry(ctx, src_stat.st_dev, src_stat.st_ino);
        if (entry != NULL) {
            return bx_cp_create_hard_link(ctx, entry->dest_path, dest_path, &src_stat, false);
        }
    }

    if (!follow_source && S_ISLNK(src_lstat.st_mode)) {
        return bx_cp_copy_symlink_object(ctx, src_path, dest_path, &src_lstat);
    }
    if (S_ISFIFO(src_stat.st_mode)) {
        if (!ctx->options->recursive || ctx->options->copy_contents) {
            return bx_cp_copy_fifo_contents(ctx, src_path, dest_path, &src_stat);
        }
        return bx_cp_copy_fifo(ctx, src_path, dest_path, &src_stat);
    }
    if (S_ISSOCK(src_stat.st_mode)) {
        if (!ctx->options->recursive || ctx->options->copy_contents) {
            return bx_cp_copy_socket_contents(ctx, src_path, dest_path, &src_stat);
        }
        return bx_cp_copy_socket(ctx, src_path, dest_path, &src_stat);
    }
    if (S_ISCHR(src_stat.st_mode) || S_ISBLK(src_stat.st_mode)) {
        if (ctx->options->copy_contents) {
            return bx_cp_copy_regular_file_path(ctx, src_path, dest_path, &src_stat);
        }
        return bx_cp_copy_device_node(ctx, src_path, dest_path, &src_stat);
    }
    if (S_ISREG(src_stat.st_mode)) {
        return bx_cp_copy_regular_file_path(ctx, src_path, dest_path, &src_stat);
    }

    bx_diag(ctx->diag, "unsupported file type for '%s'", src_path);
    return false;
}

static char *bx_cp_build_dest_path(const struct bx_cp_options *options,
                                   const char *source_operand,
                                   const char *destination_root,
                                   bool destination_is_directory) {
    return bx_path_build_dest(source_operand,
                              destination_root,
                              destination_is_directory,
                              options->parents);
}

int bx_cp_main(int argc, char **argv) {
    struct bx_cp_options options;
    int first_operand = 0;
    struct bx_cp_context ctx;
    struct bx_diag_ctx diag_ctx = {0};

    if (!bx_cp_parse_options(argc, argv, &options, &first_operand, &diag_ctx)) {
        return 1;
    }

    diag_ctx.progname = options.progname;
    diag_ctx.verbose = options.verbose;
    diag_ctx.debug = options.debug;

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

        if (options.parents && !destination_is_directory) {
            bx_diag(&diag_ctx, "--parents requires a directory destination");
            return 1;
        }
    }

    mode_t old_umask = umask(0);
    umask(old_umask);

    memset(&ctx, 0, sizeof(ctx));
    ctx.options = &options;
    ctx.diag = &diag_ctx;
    ctx.umask_value = old_umask;
    ctx.target_root = destination_root;
    bx_backup_get_params(options.backup_mode, options.suffix, &ctx.backup_params);

    for (int i = 0; i < source_count; i++) {
        char *lookup_path = xstrdup(source_operands[i]);
        char *source_operand = options.strip_trailing_slashes
                               ? bx_path_strip_trailing_slashes_dup(source_operands[i])
                               : xstrdup(source_operands[i]);
        char *dest_path = bx_cp_build_dest_path(&options, source_operand, destination_root, destination_is_directory);
        ctx.stop_current_source = false;
        ctx.current_source_root = lookup_path;
        ctx.current_dest_root = dest_path;

        bx_cp_copy_path(&ctx, lookup_path, source_operand, dest_path, true);

        free(ctx.current_dest_root_realpath);
        ctx.current_dest_root_realpath = NULL;
        ctx.current_dest_root = NULL;
        ctx.current_source_root = NULL;
        free(dest_path);
        free(source_operand);
        free(lookup_path);
    }

    bx_cp_free_links(&ctx);
    bx_cp_free_source_dirs(&ctx);
    return diag_ctx.exit_status;
}
