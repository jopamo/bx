#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "common/path_ops.h"
#include "common/same_file.h"
#include "common/stat_ops.h"
#include "diag.h"
#include "libbx.h"

enum bx_cp_deref_mode {
    BX_CP_DEREF_DEFAULT = 0,
    BX_CP_DEREF_ALWAYS,
    BX_CP_DEREF_NEVER,
    BX_CP_DEREF_COMMAND_LINE,
};

enum bx_cp_update_mode {
    BX_CP_UPDATE_ALL = 0,
    BX_CP_UPDATE_NONE,
    BX_CP_UPDATE_NONE_FAIL,
    BX_CP_UPDATE_OLDER,
};

enum {
    BX_CP_PRESERVE_MODE = 1u << 0,
    BX_CP_PRESERVE_OWNERSHIP = 1u << 1,
    BX_CP_PRESERVE_TIMESTAMPS = 1u << 2,
    BX_CP_PRESERVE_LINKS = 1u << 3,
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
    bool force;
    bool hard_link;
    bool symbolic_link;
    bool parents;
    bool remove_destination;
    bool no_target_directory;
    bool verbose;
    bool strip_trailing_slashes;
    bool show_help;
    bool show_version;
    enum bx_cp_deref_mode deref_mode;
    enum bx_cp_update_mode update_mode;
    unsigned preserve_mask;
    const char *target_directory;
};

struct bx_cp_link_entry {
    dev_t dev;
    ino_t ino;
    char *dest_path;
    struct bx_cp_link_entry *next;
};

struct bx_cp_context {
    const struct bx_cp_options *options;
    mode_t umask_value;
    struct bx_cp_link_entry *links;
    bool dest_root_active;
    dev_t dest_root_dev;
    ino_t dest_root_ino;
};

static const char *bx_cp_progname(const char *argv0) {
    return (argv0 && argv0[0] != '\0') ? argv0 : "cp";
}

static void bx_cp_vdiag(const struct bx_cp_options *options, const char *fmt, va_list ap) {
    fprintf(stderr, "%s: ", options->progname);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

static void bx_cp_diag(const struct bx_cp_options *options, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bx_cp_vdiag(options, fmt, ap);
    va_end(ap);
}

static void bx_cp_perror_path(const struct bx_cp_options *options, const char *path) {
    fprintf(stderr, "%s: %s: %s\n", options->progname, path, strerror(errno));
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
    fprintf(stream, "Not yet implemented: backup, interactive, copy-contents, reflink, sparse,\n");
    fprintf(stream, "keep-directory-symlink, one-file-system, SELinux/SMACK context handling.\n");
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

static bool bx_cp_parse_preserve_list(const struct bx_cp_options *options,
                                      const char *arg,
                                      unsigned *mask,
                                      bool set_bits) {
    char *copy = xstrdup(arg);
    char *saveptr = NULL;

    for (char *token = strtok_r(copy, ",", &saveptr);
         token != NULL;
         token = strtok_r(NULL, ",", &saveptr)) {
        unsigned bits = 0;

        if (strcmp(token, "mode") == 0) {
            bits = BX_CP_PRESERVE_MODE;
        } else if (strcmp(token, "ownership") == 0) {
            bits = BX_CP_PRESERVE_OWNERSHIP;
        } else if (strcmp(token, "timestamps") == 0) {
            bits = BX_CP_PRESERVE_TIMESTAMPS;
        } else if (strcmp(token, "links") == 0) {
            bits = BX_CP_PRESERVE_LINKS;
        } else if (strcmp(token, "all") == 0) {
            bits = BX_CP_PRESERVE_MODE | BX_CP_PRESERVE_OWNERSHIP |
                   BX_CP_PRESERVE_TIMESTAMPS | BX_CP_PRESERVE_LINKS;
        } else if (strcmp(token, "context") == 0 || strcmp(token, "xattr") == 0) {
            bits = 0;
        } else {
            bx_cp_diag(options, "invalid attribute '%s'", token);
            free(copy);
            return false;
        }

        if (set_bits) {
            *mask |= bits;
        } else {
            *mask &= ~bits;
        }
    }

    free(copy);
    return true;
}

static bool bx_cp_parse_update_mode(const struct bx_cp_options *options,
                                    const char *arg,
                                    enum bx_cp_update_mode *mode_out) {
    if (arg == NULL || strcmp(arg, "older") == 0) {
        *mode_out = BX_CP_UPDATE_OLDER;
        return true;
    }
    if (strcmp(arg, "all") == 0) {
        *mode_out = BX_CP_UPDATE_ALL;
        return true;
    }
    if (strcmp(arg, "none") == 0) {
        *mode_out = BX_CP_UPDATE_NONE;
        return true;
    }
    if (strcmp(arg, "none-fail") == 0) {
        *mode_out = BX_CP_UPDATE_NONE_FAIL;
        return true;
    }

    bx_cp_diag(options, "invalid --update mode '%s'", arg);
    return false;
}

static bool bx_cp_parse_options(int argc,
                                char **argv,
                                struct bx_cp_options *options,
                                int *first_operand) {
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
    char short_buf[] = "abdfHiLPlnpRrsS:t:TuvxZ";

    memset(options, 0, sizeof(*options));
    options->progname = bx_cp_progname(argv[0]);
    options->deref_mode = BX_CP_DEREF_DEFAULT;
    options->update_mode = BX_CP_UPDATE_ALL;

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
            options->preserve_mask |= BX_CP_PRESERVE_MODE |
                                      BX_CP_PRESERVE_OWNERSHIP |
                                      BX_CP_PRESERVE_TIMESTAMPS |
                                      BX_CP_PRESERVE_LINKS;
            break;
        case BX_CP_OPT_ATTRIBUTES_ONLY:
            options->attributes_only = true;
            break;
        case 'b':
        case 'i':
        case 'S':
        case 'x':
        case 'Z':
            bx_cp_diag(options, "option '%s' is not implemented", argv[optind - 1]);
            return false;
        case BX_CP_OPT_BACKUP:
        case BX_CP_OPT_COPY_CONTENTS:
        case BX_CP_OPT_DEBUG:
        case BX_CP_OPT_KEEP_DIRECTORY_SYMLINK:
        case BX_CP_OPT_REFLINK:
        case BX_CP_OPT_SPARSE:
        case BX_CP_OPT_CONTEXT:
            bx_cp_diag(options, "option '%s' is not implemented", argv[optind - 1]);
            return false;
        case 'd':
            options->deref_mode = BX_CP_DEREF_NEVER;
            options->preserve_mask |= BX_CP_PRESERVE_LINKS;
            break;
        case 'f':
            options->force = true;
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
        case 'l':
            options->hard_link = true;
            break;
        case 'n':
            options->update_mode = BX_CP_UPDATE_NONE;
            break;
        case 'p':
            options->preserve_mask |= BX_CP_PRESERVE_MODE |
                                      BX_CP_PRESERVE_OWNERSHIP |
                                      BX_CP_PRESERVE_TIMESTAMPS;
            break;
        case BX_CP_OPT_PRESERVE:
            if (optarg == NULL) {
                options->preserve_mask |= BX_CP_PRESERVE_MODE |
                                          BX_CP_PRESERVE_OWNERSHIP |
                                          BX_CP_PRESERVE_TIMESTAMPS;
            } else if (!bx_cp_parse_preserve_list(options, optarg, &options->preserve_mask, true)) {
                return false;
            }
            break;
        case BX_CP_OPT_NO_PRESERVE:
            if (!bx_cp_parse_preserve_list(options, optarg, &options->preserve_mask, false)) {
                return false;
            }
            break;
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
            if (!bx_cp_parse_update_mode(options, optarg, &options->update_mode)) {
                return false;
            }
            break;
        case 'u':
            options->update_mode = BX_CP_UPDATE_OLDER;
            break;
        case 'v':
            options->verbose = true;
            break;
        case '?':
            bx_cp_diag(options, "unrecognized option '%s'", argv[optind - 1]);
            return false;
        default:
            bx_cp_diag(options, "internal option parsing error");
            return false;
        }
    }

    if (options->hard_link && options->symbolic_link) {
        bx_cp_diag(options, "cannot combine --link and --symbolic-link");
        return false;
    }
    if (options->target_directory && options->no_target_directory) {
        bx_cp_diag(options, "cannot combine --target-directory and --no-target-directory");
        return false;
    }
    if (options->parents && options->no_target_directory) {
        bx_cp_diag(options, "--parents requires a target directory");
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

static bool bx_cp_should_skip_existing(const struct bx_cp_options *options,
                                       const char *dest_path,
                                       const struct stat *src_stat,
                                       const struct stat *dest_stat,
                                       bool *skip_out) {
    *skip_out = false;

    switch (options->update_mode) {
    case BX_CP_UPDATE_ALL:
        return true;
    case BX_CP_UPDATE_NONE:
        *skip_out = true;
        return true;
    case BX_CP_UPDATE_NONE_FAIL:
        bx_cp_diag(options, "will not overwrite '%s'", dest_path);
        return false;
    case BX_CP_UPDATE_OLDER:
        if (bx_stat_timespec_compare(&src_stat->st_mtim, &dest_stat->st_mtim) <= 0) {
            *skip_out = true;
        }
        return true;
    }

    return true;
}

static bool bx_cp_prepare_parents(const struct bx_cp_context *ctx, const char *path) {
    char *copy = xstrdup(path);
    size_t len = strlen(copy);
    size_t start = 0;

    if (len == 0) {
        free(copy);
        return true;
    }
    if (copy[0] == '/') {
        start = 1;
    }

    for (size_t i = start; copy[i] != '\0'; i++) {
        if (copy[i] != '/') {
            continue;
        }
        copy[i] = '\0';
        if (copy[0] != '\0') {
            struct stat st;
            if (mkdir(copy, 0777u & ~ctx->umask_value) != 0) {
                if (errno != EEXIST) {
                    bx_cp_perror_path(ctx->options, copy);
                    free(copy);
                    return false;
                }
                if (stat(copy, &st) != 0) {
                    bx_cp_perror_path(ctx->options, copy);
                    free(copy);
                    return false;
                }
                if (!S_ISDIR(st.st_mode)) {
                    bx_cp_diag(ctx->options, "cannot create directory '%s': Not a directory", copy);
                    free(copy);
                    return false;
                }
            }
        }
        copy[i] = '/';
    }

    free(copy);
    return true;
}

static bool bx_cp_apply_fd_attrs(const struct bx_cp_context *ctx,
                                 int fd,
                                 const struct stat *src_stat) {
    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_OWNERSHIP) != 0u) {
        if (fchown(fd, src_stat->st_uid, src_stat->st_gid) != 0) {
            bx_cp_perror_path(ctx->options, "fchown");
            return false;
        }
    }
    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_MODE) != 0u) {
        if (fchmod(fd, src_stat->st_mode & 07777u) != 0) {
            bx_cp_perror_path(ctx->options, "fchmod");
            return false;
        }
    }
    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        if (futimens(fd, ts) != 0) {
            bx_cp_perror_path(ctx->options, "futimens");
            return false;
        }
    }
    return true;
}

static bool bx_cp_apply_path_attrs(const struct bx_cp_context *ctx,
                                   const char *dest_path,
                                   const struct stat *src_stat,
                                   bool no_follow,
                                   bool is_directory) {
    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_OWNERSHIP) != 0u) {
        if ((no_follow ? lchown(dest_path, src_stat->st_uid, src_stat->st_gid)
                       : chown(dest_path, src_stat->st_uid, src_stat->st_gid)) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
    }

    if (!no_follow && (ctx->options->preserve_mask & BX_CP_PRESERVE_MODE) != 0u) {
        if (chmod(dest_path, src_stat->st_mode & 07777u) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
    }

    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_TIMESTAMPS) != 0u) {
        struct timespec ts[2] = {src_stat->st_atim, src_stat->st_mtim};
        int flags = no_follow ? AT_SYMLINK_NOFOLLOW : 0;
        if (utimensat(AT_FDCWD, dest_path, ts, flags) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
    }

    if (!no_follow && !is_directory && (ctx->options->preserve_mask & BX_CP_PRESERVE_MODE) == 0u) {
        /* nothing */
    }

    return true;
}

static mode_t bx_cp_directory_create_mode(const struct bx_cp_context *ctx,
                                          const struct stat *src_stat,
                                          mode_t *final_mode_out,
                                          bool *restore_mode_out) {
    mode_t source_mode = src_stat->st_mode & 0777u;

    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_MODE) != 0u) {
        *final_mode_out = 0;
        *restore_mode_out = false;
        return source_mode | S_IRWXU;
    }

    *final_mode_out = source_mode & ~ctx->umask_value;
    *restore_mode_out = (*final_mode_out | S_IRWXU) != *final_mode_out;
    return *final_mode_out | S_IRWXU;
}

static void bx_cp_print_verbose(const struct bx_cp_context *ctx,
                                const char *src_path,
                                const char *dest_path) {
    if (ctx->options->verbose) {
        printf("'%s' -> '%s'\n", src_path, dest_path);
    }
}

static bool bx_cp_copy_data(int src_fd, int dest_fd, const struct bx_cp_options *options) {
    char buffer[65536];

    while (true) {
        ssize_t nread = read(src_fd, buffer, sizeof(buffer));
        if (nread == 0) {
            return true;
        }
        if (nread < 0) {
            bx_cp_perror_path(options, "read");
            return false;
        }

        ssize_t written_total = 0;
        while (written_total < nread) {
            ssize_t nwritten = write(dest_fd,
                                     buffer + written_total,
                                     (size_t)(nread - written_total));
            if (nwritten < 0) {
                bx_cp_perror_path(options, "write");
                return false;
            }
            written_total += nwritten;
        }
    }
}

static bool bx_cp_unlink_existing_file(const struct bx_cp_context *ctx, const char *dest_path) {
    if (unlink(dest_path) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }
    return true;
}

static bool bx_cp_copy_regular_file(struct bx_cp_context *ctx,
                                    const char *src_path,
                                    const char *dest_path,
                                    const struct stat *src_stat) {
    struct bx_dest_state dest_state;
    bool skip = false;
    int src_fd = -1;
    int dest_fd = -1;
    mode_t create_mode = src_stat->st_mode & 0777u;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_cp_diag(ctx->options, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.dangling_symlink && !ctx->options->remove_destination) {
        bx_cp_diag(ctx->options, "not writing through dangling symlink '%s'", dest_path);
        return false;
    }

    if (dest_state.exists_stat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip)) {
            return false;
        }
        if (skip) {
            return true;
        }
    }

    if (dest_state.exists_lstat && S_ISDIR(dest_state.lst.st_mode)) {
        bx_cp_diag(ctx->options, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, src_path);
        return false;
    }

    if (!ctx->options->attributes_only) {
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) {
            bx_cp_perror_path(ctx->options, src_path);
            return false;
        }
    }

    if (ctx->options->remove_destination && dest_state.exists_lstat) {
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            goto fail;
        }
        memset(&dest_state, 0, sizeof(dest_state));
    }

    dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, create_mode);
    if (dest_fd < 0 && ctx->options->force && dest_state.exists_lstat) {
        if (bx_cp_unlink_existing_file(ctx, dest_path)) {
            dest_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, create_mode);
        }
    }
    if (dest_fd < 0) {
        if (dest_state.dangling_symlink && !ctx->options->force && !ctx->options->remove_destination) {
            bx_cp_diag(ctx->options, "not writing through dangling symlink '%s'", dest_path);
        } else {
            bx_cp_perror_path(ctx->options, dest_path);
        }
        goto fail;
    }

    if (!ctx->options->attributes_only && !bx_cp_copy_data(src_fd, dest_fd, ctx->options)) {
        goto fail;
    }
    if (!bx_cp_apply_fd_attrs(ctx, dest_fd, src_stat)) {
        goto fail;
    }

    if (close(dest_fd) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        dest_fd = -1;
        goto fail;
    }
    dest_fd = -1;

    if (src_fd >= 0 && close(src_fd) != 0) {
        bx_cp_perror_path(ctx->options, src_path);
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

static bool bx_cp_copy_symlink_object(struct bx_cp_context *ctx,
                                      const char *src_path,
                                      const char *dest_path,
                                      const struct stat *src_lstat) {
    struct bx_dest_state dest_state;
    bool skip = false;
    ssize_t target_size = src_lstat->st_size > 0 ? src_lstat->st_size : 256;
    char *link_target = NULL;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (dest_state.exists_lstat && bx_same_file(src_lstat, &dest_state.lst)) {
        bx_cp_diag(ctx->options, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_lstat, &dest_state.lst, &skip)) {
            return false;
        }
        if (skip) {
            return true;
        }
        if (S_ISDIR(dest_state.lst.st_mode)) {
            bx_cp_diag(ctx->options, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, src_path);
            return false;
        }
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            return false;
        }
    }

    while (true) {
        link_target = xrealloc(link_target, (size_t)target_size + 1u);
        ssize_t nread = readlink(src_path, link_target, (size_t)target_size);
        if (nread < 0) {
            free(link_target);
            bx_cp_perror_path(ctx->options, src_path);
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
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (!bx_cp_apply_path_attrs(ctx, dest_path, src_lstat, true, false)) {
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
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_cp_diag(ctx->options, "'%s' and '%s' are the same file", source_operand, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (dest_state.exists_stat) {
            if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip)) {
                return false;
            }
            if (skip) {
                return true;
            }
        }
        if (S_ISDIR(dest_state.lst.st_mode)) {
            bx_cp_diag(ctx->options, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, source_operand);
            return false;
        }
        if (!(ctx->options->force || ctx->options->remove_destination)) {
            errno = EEXIST;
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            return false;
        }
    }

    if (symlink(source_operand, dest_path) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    bx_cp_print_verbose(ctx, source_operand, dest_path);
    return true;
}

static bool bx_cp_create_hard_link(struct bx_cp_context *ctx,
                                   const char *src_path,
                                   const char *dest_path,
                                   const struct stat *src_stat) {
    struct bx_dest_state dest_state;
    bool skip = false;

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (dest_state.exists_stat && bx_same_file(src_stat, &dest_state.st)) {
        bx_cp_diag(ctx->options, "'%s' and '%s' are the same file", src_path, dest_path);
        return false;
    }

    if (dest_state.exists_stat) {
        if (!bx_cp_should_skip_existing(ctx->options, dest_path, src_stat, &dest_state.st, &skip)) {
            return false;
        }
        if (skip) {
            return true;
        }
    }

    if (dest_state.exists_lstat) {
        if (S_ISDIR(dest_state.lst.st_mode)) {
            bx_cp_diag(ctx->options, "cannot overwrite directory '%s' with non-directory '%s'", dest_path, src_path);
            return false;
        }
        if (!(ctx->options->force || ctx->options->remove_destination)) {
            errno = EEXIST;
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
        if (!bx_cp_unlink_existing_file(ctx, dest_path)) {
            return false;
        }
    }

    if (link(src_path, dest_path) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
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

    if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
        bx_cp_perror_path(ctx->options, dest_path);
        return false;
    }

    if (dest_state.exists_lstat) {
        if (!S_ISDIR(dest_state.lst.st_mode)) {
            bx_cp_diag(ctx->options, "cannot overwrite non-directory '%s' with directory '%s'", dest_path, src_path);
            return false;
        }
    } else {
        mode_t mkdir_mode = bx_cp_directory_create_mode(ctx, src_stat, &final_mode, &restore_mode);
        if (mkdir(dest_path, mkdir_mode) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
        created = true;
        if (bx_stat_collect_dest_state(dest_path, &dest_state) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            return false;
        }
    }

    if (top_level) {
        ctx->dest_root_active = true;
        ctx->dest_root_dev = dest_state.lst.st_dev;
        ctx->dest_root_ino = dest_state.lst.st_ino;
    }

    DIR *dir = opendir(src_path);
    if (dir == NULL) {
        bx_cp_perror_path(ctx->options, src_path);
        ctx->dest_root_active = prev_dest_root_active;
        ctx->dest_root_dev = prev_dest_root_dev;
        ctx->dest_root_ino = prev_dest_root_ino;
        return false;
    }

    bool ok = true;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_cp_perror_path(ctx->options, src_path);
                ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }

        char *src_child = bx_path_join(src_path, entry->d_name);
        struct stat child_lstat;
        if (lstat(src_child, &child_lstat) != 0) {
            bx_cp_perror_path(ctx->options, src_child);
            free(src_child);
            ok = false;
            continue;
        }
        if (ctx->dest_root_active &&
            S_ISDIR(child_lstat.st_mode) &&
            child_lstat.st_dev == ctx->dest_root_dev &&
            child_lstat.st_ino == ctx->dest_root_ino) {
            bx_cp_diag(ctx->options, "cannot copy a directory, '%s', into itself, '%s'", src_path, dest_path);
            free(src_child);
            ok = false;
            continue;
        }

        char *dest_child = bx_path_join(dest_path, entry->d_name);
        if (!bx_cp_copy_path(ctx, src_child, src_child, dest_child, false)) {
            ok = false;
        }
        free(dest_child);
        free(src_child);
    }

    if (closedir(dir) != 0) {
        bx_cp_perror_path(ctx->options, src_path);
        ok = false;
    }

    if (ok && created && restore_mode) {
        if (chmod(dest_path, final_mode) != 0) {
            bx_cp_perror_path(ctx->options, dest_path);
            ok = false;
        }
    }
    if (ok && !bx_cp_apply_path_attrs(ctx, dest_path, src_stat, false, true)) {
        ok = false;
    }
    if (ok && created) {
        bx_cp_print_verbose(ctx, src_path, dest_path);
    }

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
        bx_cp_perror_path(ctx->options, src_path);
        return false;
    }

    source_is_symlink = S_ISLNK(src_lstat.st_mode);
    follow_source = bx_cp_should_follow_source(ctx->options, top_level, source_is_symlink);

    if (follow_source) {
        if (stat(src_path, &src_stat) != 0) {
            bx_cp_perror_path(ctx->options, src_path);
            return false;
        }
    } else {
        src_stat = src_lstat;
    }

    if (ctx->options->parents && !bx_cp_prepare_parents(ctx, dest_path)) {
        return false;
    }

    if (S_ISDIR(src_stat.st_mode)) {
        if (!ctx->options->recursive) {
            bx_cp_diag(ctx->options, "-r not specified; omitting directory '%s'", src_path);
            return false;
        }
        return bx_cp_copy_directory(ctx, src_path, dest_path, &src_stat, top_level);
    }

    if ((ctx->options->preserve_mask & BX_CP_PRESERVE_LINKS) != 0u &&
        !ctx->options->hard_link &&
        !ctx->options->symbolic_link) {
        struct bx_cp_link_entry *entry = bx_cp_find_link_entry(ctx, src_stat.st_dev, src_stat.st_ino);
        if (entry != NULL) {
            return bx_cp_create_hard_link(ctx, entry->dest_path, dest_path, &src_stat);
        }
    }

    if (!follow_source && S_ISLNK(src_lstat.st_mode)) {
        return bx_cp_copy_symlink_object(ctx, src_path, dest_path, &src_lstat);
    }

    if (ctx->options->symbolic_link) {
        return bx_cp_create_symbolic_link(ctx, source_operand, dest_path, &src_stat);
    }
    if (ctx->options->hard_link) {
        return bx_cp_create_hard_link(ctx, src_path, dest_path, &src_stat);
    }
    if (S_ISREG(src_stat.st_mode)) {
        return bx_cp_copy_regular_file(ctx, src_path, dest_path, &src_stat);
    }

    bx_cp_diag(ctx->options, "unsupported file type for '%s'", src_path);
    return false;
}

static char *bx_cp_build_dest_path(const struct bx_cp_options *options,
                                   const char *source_operand,
                                   const char *destination_root,
                                   bool destination_is_directory) {
    if (options->parents) {
        char *parents_path = bx_path_parents_layout_dup(source_operand);
        char *res = bx_path_join(destination_root, parents_path);
        free(parents_path);
        return res;
    }

    if (destination_is_directory) {
        char *base = bx_path_basename_dup(source_operand);
        char *res = bx_path_join(destination_root, base);
        free(base);
        return res;
    }

    return xstrdup(destination_root);
}

int bx_cp_main(int argc, char **argv) {
    struct bx_cp_options options;
    int first_operand = 0;
    struct bx_cp_context ctx;
    int rc = 0;

    if (!bx_cp_parse_options(argc, argv, &options, &first_operand)) {
        return 1;
    }

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
        bx_cp_diag(&options, "missing file operand");
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
            bx_cp_diag(&options, "target '%s' is not a directory", destination_root);
            return 1;
        }
        destination_is_directory = true;
    } else {
        if (operand_count < 2) {
            bx_cp_diag(&options, "missing destination file operand after '%s'", argv[first_operand]);
            return 1;
        }
        destination_root = argv[argc - 1];
        source_operands = argv + first_operand;
        source_count = operand_count - 1;

        if (source_count > 1) {
            if (options.no_target_directory) {
                bx_cp_diag(&options, "extra operand '%s'", argv[first_operand + 1]);
                return 1;
            }
            if (!bx_stat_is_dir_path(destination_root)) {
                bx_cp_diag(&options, "target '%s' is not a directory", destination_root);
                return 1;
            }
            destination_is_directory = true;
        } else if (!options.no_target_directory && bx_stat_is_dir_path(destination_root)) {
            destination_is_directory = true;
        }

        if (options.parents && !destination_is_directory) {
            bx_cp_diag(&options, "--parents requires a directory destination");
            return 1;
        }
    }

    mode_t old_umask = umask(0);
    umask(old_umask);

    memset(&ctx, 0, sizeof(ctx));
    ctx.options = &options;
    ctx.umask_value = old_umask;

    for (int i = 0; i < source_count; i++) {
        char *lookup_path = xstrdup(source_operands[i]);
        char *source_operand = options.strip_trailing_slashes
                               ? bx_path_strip_trailing_slashes_dup(source_operands[i])
                               : xstrdup(source_operands[i]);
        char *dest_path = bx_cp_build_dest_path(&options, source_operand, destination_root, destination_is_directory);

        if (!bx_cp_copy_path(&ctx, lookup_path, source_operand, dest_path, true)) {
            rc = 1;
        }

        free(dest_path);
        free(source_operand);
        free(lookup_path);
    }

    bx_cp_free_links(&ctx);
    return rc;
}
