#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

#ifndef MS_MANDLOCK
#define MS_MANDLOCK 0ul
#endif

#ifndef MS_DIRSYNC
#define MS_DIRSYNC 0ul
#endif

#ifndef MS_RELATIME
#define MS_RELATIME 0ul
#endif

#ifndef MS_STRICTATIME
#define MS_STRICTATIME 0ul
#endif

#ifndef MS_LAZYTIME
#define MS_LAZYTIME 0ul
#endif

#ifndef MS_SILENT
#define MS_SILENT 0ul
#endif

struct bx_mount_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool no_mtab;
    const char* fstype;
    char* data;
    unsigned long op_flags;
    unsigned long flag_values;
    unsigned long flag_mentions;
    int operand_index;
    const char* source;
    const char* target;
};

static const char* bx_mount_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "mount";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_mount_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... -t TYPE SOURCE TARGET\n", progname);
    fprintf(stream, "       %s [OPTION]... --bind SOURCE TARGET\n", progname);
    fprintf(stream, "       %s [OPTION]... --move SOURCE TARGET\n", progname);
    fprintf(stream, "       %s [OPTION]... --remount TARGET\n", progname);
    fprintf(stream, "       %s [OPTION]... --remount SOURCE TARGET\n", progname);
    fprintf(stream, "Mount a filesystem using explicit operands only.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -t, --types=TYPE      filesystem type (single type only)\n");
    fprintf(stream, "      --type=TYPE       alias for --types\n");
    fprintf(stream, "  -o, --options=LIST    comma-separated mount options\n");
    fprintf(stream, "  -r, --read-only       mount read-only\n");
    fprintf(stream, "  -w, --read-write      mount read-write\n");
    fprintf(stream, "      --rw              alias for --read-write\n");
    fprintf(stream, "  -n, --no-mtab         accepted for compatibility; ignored\n");
    fprintf(stream, "      --bind            create a bind mount\n");
    fprintf(stream, "      --rbind           create a recursive bind mount\n");
    fprintf(stream, "      --move            move an existing mount\n");
    fprintf(stream, "      --remount         remount an existing mount\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase does not implement /etc/fstab, loop devices, helper programs,\n");
    fprintf(stream, "or automount-style behavior.\n");
}

static void bx_mount_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_mount_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_mount_set_flag(struct bx_mount_options* options, unsigned long flag) {
    options->flag_values |= flag;
    options->flag_mentions |= flag;
}

static void bx_mount_clear_flag(struct bx_mount_options* options, unsigned long flag) {
    options->flag_values &= ~flag;
    options->flag_mentions |= flag;
}

static char* bx_mount_trim_token(char* text) {
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }

    return text;
}

static void bx_mount_append_data_option(struct bx_mount_options* options, const char* token) {
    if (token == NULL || token[0] == '\0') {
        return;
    }

    if (options->data == NULL || options->data[0] == '\0') {
        free(options->data);
        options->data = xstrdup(token);
        return;
    }

    size_t old_len = strlen(options->data);
    size_t token_len = strlen(token);
    char* resized = xrealloc(options->data, old_len + 1u + token_len + 1u);
    resized[old_len] = ',';
    memcpy(resized + old_len + 1u, token, token_len + 1u);
    options->data = resized;
}

static bool bx_mount_parse_option_token(const char* token, struct bx_mount_options* options) {
    if (strcmp(token, "defaults") == 0) {
        return true;
    }

    if (strcmp(token, "ro") == 0) {
        bx_mount_set_flag(options, MS_RDONLY);
        return true;
    }
    if (strcmp(token, "rw") == 0) {
        bx_mount_clear_flag(options, MS_RDONLY);
        return true;
    }
    if (strcmp(token, "nosuid") == 0) {
        bx_mount_set_flag(options, MS_NOSUID);
        return true;
    }
    if (strcmp(token, "suid") == 0) {
        bx_mount_clear_flag(options, MS_NOSUID);
        return true;
    }
    if (strcmp(token, "nodev") == 0) {
        bx_mount_set_flag(options, MS_NODEV);
        return true;
    }
    if (strcmp(token, "dev") == 0) {
        bx_mount_clear_flag(options, MS_NODEV);
        return true;
    }
    if (strcmp(token, "noexec") == 0) {
        bx_mount_set_flag(options, MS_NOEXEC);
        return true;
    }
    if (strcmp(token, "exec") == 0) {
        bx_mount_clear_flag(options, MS_NOEXEC);
        return true;
    }
    if (strcmp(token, "sync") == 0) {
        bx_mount_set_flag(options, MS_SYNCHRONOUS);
        return true;
    }
    if (strcmp(token, "async") == 0) {
        bx_mount_clear_flag(options, MS_SYNCHRONOUS);
        return true;
    }
    if (strcmp(token, "dirsync") == 0) {
        bx_mount_set_flag(options, MS_DIRSYNC);
        return true;
    }
    if (strcmp(token, "mand") == 0) {
        bx_mount_set_flag(options, MS_MANDLOCK);
        return true;
    }
    if (strcmp(token, "nomand") == 0) {
        bx_mount_clear_flag(options, MS_MANDLOCK);
        return true;
    }
    if (strcmp(token, "atime") == 0) {
        bx_mount_clear_flag(options, MS_NOATIME);
        bx_mount_clear_flag(options, MS_NODIRATIME);
        return true;
    }
    if (strcmp(token, "noatime") == 0) {
        bx_mount_set_flag(options, MS_NOATIME);
        return true;
    }
    if (strcmp(token, "diratime") == 0) {
        bx_mount_clear_flag(options, MS_NODIRATIME);
        return true;
    }
    if (strcmp(token, "nodiratime") == 0) {
        bx_mount_set_flag(options, MS_NODIRATIME);
        return true;
    }
    if (strcmp(token, "relatime") == 0) {
        bx_mount_clear_flag(options, MS_STRICTATIME);
        bx_mount_set_flag(options, MS_RELATIME);
        return true;
    }
    if (strcmp(token, "norelatime") == 0) {
        bx_mount_clear_flag(options, MS_RELATIME);
        return true;
    }
    if (strcmp(token, "strictatime") == 0) {
        bx_mount_clear_flag(options, MS_RELATIME);
        bx_mount_set_flag(options, MS_STRICTATIME);
        return true;
    }
    if (strcmp(token, "lazytime") == 0) {
        bx_mount_set_flag(options, MS_LAZYTIME);
        return true;
    }
    if (strcmp(token, "nolazytime") == 0) {
        bx_mount_clear_flag(options, MS_LAZYTIME);
        return true;
    }
    if (strcmp(token, "silent") == 0) {
        bx_mount_set_flag(options, MS_SILENT);
        return true;
    }
    if (strcmp(token, "loud") == 0) {
        bx_mount_clear_flag(options, MS_SILENT);
        return true;
    }
    if (strcmp(token, "bind") == 0) {
        options->op_flags |= MS_BIND;
        return true;
    }
    if (strcmp(token, "rbind") == 0) {
        options->op_flags |= MS_BIND | MS_REC;
        return true;
    }
    if (strcmp(token, "move") == 0) {
        options->op_flags |= MS_MOVE;
        return true;
    }
    if (strcmp(token, "remount") == 0) {
        options->op_flags |= MS_REMOUNT;
        return true;
    }
    if (strcmp(token, "rec") == 0) {
        options->op_flags |= MS_REC;
        return true;
    }

    bx_mount_append_data_option(options, token);
    return true;
}

static bool bx_mount_parse_option_list(const char* text, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "missing mount option list");
        return false;
    }

    char* copy = xstrdup(text);
    char* cursor = copy;

    while (true) {
        char* token = cursor;
        char* comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        }

        token = bx_mount_trim_token(token);
        if (token[0] != '\0') {
            bx_mount_parse_option_token(token, options);
        }

        if (comma == NULL) {
            break;
        }
    }

    free(copy);
    return true;
}

static bool bx_mount_parse_fstype(const char* text, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL || text[0] == '\0') {
        bx_diag(diag, "filesystem type may not be empty");
        return false;
    }
    if (strchr(text, ',') != NULL) {
        bx_diag(diag, "multiple filesystem types are not supported in this phase");
        return false;
    }

    options->fstype = text;
    return true;
}

static bool bx_mount_parse_options(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    enum {
        BX_MOUNT_OPT_BIND = 1,
        BX_MOUNT_OPT_RBIND,
        BX_MOUNT_OPT_MOVE,
        BX_MOUNT_OPT_REMOUNT,
        BX_MOUNT_OPT_HELP,
        BX_MOUNT_OPT_VERSION,
    };

    static const struct option long_options[] = {
        {"types", required_argument, NULL, 't'},
        {"type", required_argument, NULL, 't'},
        {"options", required_argument, NULL, 'o'},
        {"read-only", no_argument, NULL, 'r'},
        {"read-write", no_argument, NULL, 'w'},
        {"rw", no_argument, NULL, 'w'},
        {"no-mtab", no_argument, NULL, 'n'},
        {"bind", no_argument, NULL, BX_MOUNT_OPT_BIND},
        {"rbind", no_argument, NULL, BX_MOUNT_OPT_RBIND},
        {"move", no_argument, NULL, BX_MOUNT_OPT_MOVE},
        {"remount", no_argument, NULL, BX_MOUNT_OPT_REMOUNT},
        {"help", no_argument, NULL, BX_MOUNT_OPT_HELP},
        {"version", no_argument, NULL, BX_MOUNT_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_mount_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+:o:t:rwn", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'o':
                if (!bx_mount_parse_option_list(optarg, options, diag)) {
                    return false;
                }
                break;
            case 't':
                if (!bx_mount_parse_fstype(optarg, options, diag)) {
                    return false;
                }
                break;
            case 'r':
                bx_mount_set_flag(options, MS_RDONLY);
                break;
            case 'w':
                bx_mount_clear_flag(options, MS_RDONLY);
                break;
            case 'n':
                options->no_mtab = true;
                break;
            case BX_MOUNT_OPT_BIND:
                options->op_flags |= MS_BIND;
                break;
            case BX_MOUNT_OPT_RBIND:
                options->op_flags |= MS_BIND | MS_REC;
                break;
            case BX_MOUNT_OPT_MOVE:
                options->op_flags |= MS_MOVE;
                break;
            case BX_MOUNT_OPT_REMOUNT:
                options->op_flags |= MS_REMOUNT;
                break;
            case BX_MOUNT_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_MOUNT_OPT_VERSION:
                options->show_version = true;
                return true;
            case ':':
                if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "option requires an argument");
                }
                return false;
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

    options->operand_index = optind;
    return true;
}

static bool bx_mount_data_present(const struct bx_mount_options* options) {
    return options->data != NULL && options->data[0] != '\0';
}

static bool bx_mount_has_bind(const struct bx_mount_options* options) {
    return (options->op_flags & MS_BIND) != 0ul;
}

static bool bx_mount_has_move(const struct bx_mount_options* options) {
    return (options->op_flags & MS_MOVE) != 0ul;
}

static bool bx_mount_has_remount(const struct bx_mount_options* options) {
    return (options->op_flags & MS_REMOUNT) != 0ul;
}

static bool bx_mount_has_rec(const struct bx_mount_options* options) {
    return (options->op_flags & MS_REC) != 0ul;
}

static bool bx_mount_validate_operand(const char* label, const char* value, struct bx_diag_ctx* diag) {
    if (value == NULL || value[0] == '\0') {
        bx_diag(diag, "%s may not be empty", label);
        return false;
    }
    return true;
}

static bool bx_mount_assign_pair_operands(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;
    if (remaining <= 0) {
        bx_diag(diag, "missing source and target operands");
        return false;
    }
    if (remaining == 1) {
        bx_diag(diag, "missing target operand after '%s'", argv[options->operand_index]);
        return false;
    }
    if (remaining > 2) {
        bx_diag(diag, "too many operands");
        return false;
    }

    options->source = argv[options->operand_index];
    options->target = argv[options->operand_index + 1];
    return bx_mount_validate_operand("source", options->source, diag) && bx_mount_validate_operand("target", options->target, diag);
}

static bool bx_mount_assign_remount_operands(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;
    if (remaining <= 0) {
        bx_diag(diag, "missing target operand");
        return false;
    }
    if (remaining > 2) {
        bx_diag(diag, "too many operands");
        return false;
    }

    if (remaining == 1) {
        options->source = NULL;
        options->target = argv[options->operand_index];
    }
    else {
        options->source = argv[options->operand_index];
        options->target = argv[options->operand_index + 1];
        if (!bx_mount_validate_operand("source", options->source, diag)) {
            return false;
        }
    }

    return bx_mount_validate_operand("target", options->target, diag);
}

static bool bx_mount_validate_request(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    bool has_bind = bx_mount_has_bind(options);
    bool has_move = bx_mount_has_move(options);
    bool has_remount = bx_mount_has_remount(options);
    bool has_rec = bx_mount_has_rec(options);

    if (has_rec && !has_bind) {
        bx_diag(diag, "recursive mount option requires a bind mount in this phase");
        return false;
    }

    if (has_move && (has_bind || has_remount)) {
        bx_diag(diag, "conflicting mount operations specified");
        return false;
    }

    if (has_move) {
        if (options->fstype != NULL) {
            bx_diag(diag, "filesystem type is not used with move mounts");
            return false;
        }
        if (bx_mount_data_present(options)) {
            bx_diag(diag, "move mounts do not accept filesystem-specific options");
            return false;
        }
        if (options->flag_mentions != 0ul) {
            bx_diag(diag, "move mounts do not accept mount flag changes");
            return false;
        }
        return bx_mount_assign_pair_operands(argc, argv, options, diag);
    }

    if (has_bind && !has_remount) {
        if (options->fstype != NULL) {
            bx_diag(diag, "filesystem type is not used with bind mounts");
            return false;
        }
        if (bx_mount_data_present(options)) {
            bx_diag(diag, "bind mounts do not accept filesystem-specific options in this phase");
            return false;
        }
        if (options->flag_mentions != 0ul) {
            bx_diag(diag, "bind mounts cannot change mount flags in one step; use a remount");
            return false;
        }
        return bx_mount_assign_pair_operands(argc, argv, options, diag);
    }

    if (has_remount) {
        if (has_bind && has_rec) {
            bx_diag(diag, "recursive bind remount is not supported in this phase");
            return false;
        }
        if (has_bind && options->fstype != NULL) {
            bx_diag(diag, "filesystem type is not used with bind remounts");
            return false;
        }
        if (has_bind && bx_mount_data_present(options)) {
            bx_diag(diag, "bind remounts do not accept filesystem-specific options in this phase");
            return false;
        }
        return bx_mount_assign_remount_operands(argc, argv, options, diag);
    }

    if (options->fstype == NULL) {
        bx_diag(diag, "filesystem type required for regular mounts; use -t TYPE");
        return false;
    }

    return bx_mount_assign_pair_operands(argc, argv, options, diag);
}

static void bx_mount_diag_failure(const struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    int mount_error = errno;

    if (bx_mount_has_move(options)) {
        bx_diag(diag, "cannot move mount from '%s' to '%s': %s", options->source, options->target, strerror(mount_error));
        return;
    }

    if (bx_mount_has_bind(options) && bx_mount_has_remount(options)) {
        if (options->source != NULL) {
            bx_diag(diag, "cannot remount bind mount '%s' on '%s': %s", options->source, options->target, strerror(mount_error));
        }
        else {
            bx_diag(diag, "cannot remount bind mount at '%s': %s", options->target, strerror(mount_error));
        }
        return;
    }

    if (bx_mount_has_bind(options)) {
        if (bx_mount_has_rec(options)) {
            bx_diag(diag, "cannot recursively bind mount '%s' on '%s': %s", options->source, options->target, strerror(mount_error));
        }
        else {
            bx_diag(diag, "cannot bind mount '%s' on '%s': %s", options->source, options->target, strerror(mount_error));
        }
        return;
    }

    if (bx_mount_has_remount(options)) {
        if (options->source != NULL && options->fstype != NULL) {
            bx_diag(diag, "cannot remount '%s' on '%s' as '%s': %s", options->source, options->target, options->fstype, strerror(mount_error));
        }
        else if (options->source != NULL) {
            bx_diag(diag, "cannot remount '%s' on '%s': %s", options->source, options->target, strerror(mount_error));
        }
        else if (options->fstype != NULL) {
            bx_diag(diag, "cannot remount '%s' as '%s': %s", options->target, options->fstype, strerror(mount_error));
        }
        else {
            bx_diag(diag, "cannot remount '%s': %s", options->target, strerror(mount_error));
        }
        return;
    }

    bx_diag(diag, "cannot mount '%s' on '%s' as '%s': %s", options->source, options->target, options->fstype, strerror(mount_error));
}

static bool bx_mount_perform(const struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    const char* source = options->source;
    const char* fstype = options->fstype;
    const void* data = bx_mount_data_present(options) ? options->data : NULL;
    unsigned long flags = options->op_flags | options->flag_values;

    if (bx_mount_has_bind(options) || bx_mount_has_move(options)) {
        fstype = NULL;
        data = NULL;
    }

    if (mount(source, options->target, fstype, flags, data) != 0) {
        bx_mount_diag_failure(options, diag);
        return false;
    }

    return true;
}

int bx_mount_main(int argc, char** argv) {
    struct bx_mount_options options;
    struct bx_diag_ctx diag = {
        .progname = "mount",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_mount_parse_options(argc, argv, &options, &diag)) {
        free(options.data);
        bx_mount_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_mount_print_help(stdout, options.progname);
        free(options.data);
        return 0;
    }

    if (options.show_version) {
        bx_mount_print_version(options.progname);
        free(options.data);
        return 0;
    }

    if (!bx_mount_validate_request(argc, argv, &options, &diag)) {
        free(options.data);
        bx_mount_print_try_help(options.progname);
        return 1;
    }

    if (!bx_mount_perform(&options, &diag)) {
        free(options.data);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    free(options.data);
    return 0;
}
