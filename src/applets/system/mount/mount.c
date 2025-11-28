#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/mount_table.h"

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
    bool verbose;
    const char* fstype;
    const char* source;
    const char* target;
    char* owned_source;
    char* data;
    unsigned long op_flags;
    unsigned long flag_values;
    unsigned long flag_mentions;
    bool explicit_source;
    bool explicit_target;
    int operand_index;
};

static void bx_mount_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-hV]\n", progname);
    fprintf(stream, "       %s [options]\n", progname);
    fprintf(stream, "       %s [options] [--source] <source> | [--target] <directory>\n", progname);
    fprintf(stream, "       %s [options] <source> <directory>\n", progname);
    fprintf(stream, "       %s <operation> <mountpoint> [<target>]\n", progname);
    fprintf(stream, "Mount a filesystem.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported subset:\n");
    fprintf(stream, "  -t, --types TYPE      filesystem type\n");
    fprintf(stream, "  -o, --options LIST    comma-separated mount options\n");
    fprintf(stream, "  -r, --read-only       mount read-only\n");
    fprintf(stream, "  -w, --read-write      mount read-write\n");
    fprintf(stream, "  -B, --bind            bind mount\n");
    fprintf(stream, "  -R, --rbind           recursive bind mount\n");
    fprintf(stream, "  -M, --move            move mount\n");
    fprintf(stream, "      --remount         remount existing mount\n");
    fprintf(stream, "      --source SRC      explicitly specify source\n");
    fprintf(stream, "      --target DIR      explicitly specify target\n");
    fprintf(stream, "  -L, --label LABEL     resolve source from /dev/disk/by-label\n");
    fprintf(stream, "  -U, --uuid UUID       resolve source from /dev/disk/by-uuid\n");
    fprintf(stream, "  -v, --verbose         say what is being done\n");
    fprintf(stream, "  -n, --no-mtab         accepted for compatibility; ignored\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "With no operands, print mounted filesystems from /proc/self/mounts.\n");
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
        BX_MOUNT_OPT_SOURCE,
        BX_MOUNT_OPT_TARGET,
    };

    static const struct option long_options[] = {
        {"types", required_argument, NULL, 't'},
        {"type", required_argument, NULL, 't'},
        {"options", required_argument, NULL, 'o'},
        {"read-only", no_argument, NULL, 'r'},
        {"ro", no_argument, NULL, 'r'},
        {"read-write", no_argument, NULL, 'w'},
        {"rw", no_argument, NULL, 'w'},
        {"no-mtab", no_argument, NULL, 'n'},
        {"bind", no_argument, NULL, BX_MOUNT_OPT_BIND},
        {"rbind", no_argument, NULL, BX_MOUNT_OPT_RBIND},
        {"move", no_argument, NULL, BX_MOUNT_OPT_MOVE},
        {"remount", no_argument, NULL, BX_MOUNT_OPT_REMOUNT},
        {"source", required_argument, NULL, BX_MOUNT_OPT_SOURCE},
        {"target", required_argument, NULL, BX_MOUNT_OPT_TARGET},
        {"label", required_argument, NULL, 'L'},
        {"uuid", required_argument, NULL, 'U'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, BX_MOUNT_OPT_HELP},
        {"version", no_argument, NULL, BX_MOUNT_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "mount");
    diag->progname = options->progname;
    diag->verbose = false;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+:BMo:t:rwnRL:U:v", long_options, NULL);
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
            case 'B':
            case BX_MOUNT_OPT_BIND:
                options->op_flags |= MS_BIND;
                break;
            case 'R':
            case BX_MOUNT_OPT_RBIND:
                options->op_flags |= MS_BIND | MS_REC;
                break;
            case 'M':
            case BX_MOUNT_OPT_MOVE:
                options->op_flags |= MS_MOVE;
                break;
            case BX_MOUNT_OPT_REMOUNT:
                options->op_flags |= MS_REMOUNT;
                break;
            case BX_MOUNT_OPT_SOURCE:
                options->source = optarg;
                options->explicit_source = true;
                break;
            case BX_MOUNT_OPT_TARGET:
                options->target = optarg;
                options->explicit_target = true;
                break;
            case 'L':
            {
                size_t len = strlen(optarg);
                char* tagged = xmalloc(6u + len + 1u);
                memcpy(tagged, "LABEL=", 6u);
                memcpy(tagged + 6u, optarg, len + 1u);
                free(options->owned_source);
                options->owned_source = tagged;
                options->source = options->owned_source;
                options->explicit_source = true;
                break;
            }
            case 'U':
            {
                size_t len = strlen(optarg);
                char* tagged = xmalloc(5u + len + 1u);
                memcpy(tagged, "UUID=", 5u);
                memcpy(tagged + 5u, optarg, len + 1u);
                free(options->owned_source);
                options->owned_source = tagged;
                options->source = options->owned_source;
                options->explicit_source = true;
                break;
            }
            case 'v':
                options->verbose = true;
                diag->verbose = true;
                break;
            case BX_MOUNT_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_MOUNT_OPT_VERSION:
                options->show_version = true;
                return true;
            case ':':
                bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                return false;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
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

static bool bx_mount_type_matches(const char* pattern, const char* type) {
    if (pattern == NULL || pattern[0] == '\0') {
        return true;
    }
    if (type == NULL) {
        return false;
    }

    char* copy = xstrdup(pattern);
    char* cursor = copy;
    bool saw_positive = false;
    bool matched_positive = false;
    bool excluded = false;

    while (cursor != NULL && *cursor != '\0') {
        char* token = cursor;
        char* comma = strchr(cursor, ',');
        if (comma != NULL) {
            *comma = '\0';
            cursor = comma + 1;
        }
        else {
            cursor = NULL;
        }

        token = bx_mount_trim_token(token);
        if (token[0] == '\0') {
            continue;
        }

        bool negated = strncmp(token, "no", 2) == 0 && token[2] != '\0';
        const char* probe = negated ? token + 2 : token;

        if (negated) {
            if (strcmp(probe, type) == 0) {
                excluded = true;
            }
        }
        else {
            saw_positive = true;
            if (strcmp(probe, type) == 0) {
                matched_positive = true;
            }
        }
    }

    free(copy);
    if (excluded) {
        return false;
    }
    return !saw_positive || matched_positive;
}

static void bx_mount_safe_fputs(const char* text) {
    for (const unsigned char* p = (const unsigned char*)text; p != NULL && *p != '\0'; p++) {
        fputc(iscntrl(*p) ? '?' : (int)*p, stdout);
    }
}

static void bx_mount_print_one_entry(const struct bx_mount_entry* entry) {
    printf("%s on ", entry->source);
    bx_mount_safe_fputs(entry->target);
    printf(" type %s", entry->fstype);
    if (entry->options != NULL && entry->options[0] != '\0') {
        printf(" (%s)", entry->options);
    }
    fputc('\n', stdout);
}

static int bx_mount_print_mounts(const struct bx_mount_options* options, const char* single_target, struct bx_diag_ctx* diag) {
    struct bx_mount_table table;
    if (!bx_mount_table_load(&table)) {
        bx_diag(diag, "failed to read /proc/self/mounts: %s", strerror(errno));
        return 1;
    }

    int rc = 0;
    bool found = false;

    if (single_target != NULL) {
        const struct bx_mount_entry* entry = bx_mount_table_find_target(&table, single_target);
        if (entry == NULL) {
            bx_diag(diag, "nothing mounted on '%s'", single_target);
            rc = 1;
        }
        else if (bx_mount_type_matches(options->fstype, entry->fstype)) {
            bx_mount_print_one_entry(entry);
            found = true;
        }
        else {
            bx_diag(diag, "nothing mounted on '%s' matching type '%s'", single_target, options->fstype);
            rc = 1;
        }
    }
    else {
        for (size_t i = 0; i < table.len; i++) {
            if (!bx_mount_type_matches(options->fstype, table.entries[i].fstype)) {
                continue;
            }
            bx_mount_print_one_entry(&table.entries[i]);
            found = true;
        }
    }

    bx_mount_table_free(&table);
    if (!found && single_target == NULL && rc == 0) {
        return 0;
    }
    return rc;
}

static char* bx_mount_resolve_disk_tag(const char* directory, const char* value) {
    size_t dir_len = strlen(directory);
    size_t value_len = strlen(value);
    char* path = xmalloc(dir_len + 1u + value_len + 1u);
    memcpy(path, directory, dir_len);
    path[dir_len] = '/';
    memcpy(path + dir_len + 1u, value, value_len + 1u);

    char* resolved = realpath(path, NULL);
    free(path);
    return resolved;
}

static char* bx_mount_resolve_source_spec(const char* source) {
    if (source == NULL) {
        return NULL;
    }

    if (strncmp(source, "LABEL=", 6) == 0) {
        return bx_mount_resolve_disk_tag("/dev/disk/by-label", source + 6);
    }
    if (strncmp(source, "UUID=", 5) == 0) {
        return bx_mount_resolve_disk_tag("/dev/disk/by-uuid", source + 5);
    }
    if (strncmp(source, "PARTLABEL=", 10) == 0) {
        return bx_mount_resolve_disk_tag("/dev/disk/by-partlabel", source + 10);
    }
    if (strncmp(source, "PARTUUID=", 9) == 0) {
        return bx_mount_resolve_disk_tag("/dev/disk/by-partuuid", source + 9);
    }

    return xstrdup(source);
}

static bool bx_mount_assign_regular_operands(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;

    if (options->explicit_source && options->explicit_target) {
        if (remaining != 0) {
            bx_diag(diag, "unexpected operand '%s'", argv[options->operand_index]);
            return false;
        }
    }
    else if (options->explicit_source) {
        if (remaining <= 0) {
            bx_diag(diag, "missing target operand");
            return false;
        }
        if (remaining > 1) {
            bx_diag(diag, "too many operands");
            return false;
        }
        options->target = argv[options->operand_index];
    }
    else if (options->explicit_target) {
        if (remaining <= 0) {
            bx_diag(diag, "missing source operand");
            return false;
        }
        if (remaining > 1) {
            bx_diag(diag, "too many operands");
            return false;
        }
        options->source = argv[options->operand_index];
    }
    else {
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
    }

    return bx_mount_validate_operand("source", options->source, diag) && bx_mount_validate_operand("target", options->target, diag);
}

static bool bx_mount_assign_remount_operands(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;

    if (options->explicit_source && options->explicit_target) {
        if (remaining != 0) {
            bx_diag(diag, "unexpected operand '%s'", argv[options->operand_index]);
            return false;
        }
    }
    else if (options->explicit_source) {
        if (remaining <= 0) {
            bx_diag(diag, "missing target operand");
            return false;
        }
        if (remaining > 1) {
            bx_diag(diag, "too many operands");
            return false;
        }
        options->target = argv[options->operand_index];
    }
    else if (options->explicit_target) {
        if (remaining > 0) {
            bx_diag(diag, "unexpected operand '%s'", argv[options->operand_index]);
            return false;
        }
    }
    else {
        if (remaining <= 0) {
            bx_diag(diag, "missing target operand");
            return false;
        }
        if (remaining > 2) {
            bx_diag(diag, "too many operands");
            return false;
        }

        if (remaining == 1) {
            options->target = argv[options->operand_index];
        }
        else {
            options->source = argv[options->operand_index];
            options->target = argv[options->operand_index + 1];
        }
    }

    if (options->source != NULL && !bx_mount_validate_operand("source", options->source, diag)) {
        return false;
    }
    return bx_mount_validate_operand("target", options->target, diag);
}

static bool bx_mount_validate_request(int argc, char** argv, struct bx_mount_options* options, struct bx_diag_ctx* diag, bool* print_only, const char** single_target) {
    bool has_bind = bx_mount_has_bind(options);
    bool has_move = bx_mount_has_move(options);
    bool has_remount = bx_mount_has_remount(options);
    bool has_rec = bx_mount_has_rec(options);
    int remaining = argc - options->operand_index;

    *print_only = false;
    *single_target = NULL;

    if (!options->explicit_source && !options->explicit_target && remaining == 0 && !has_bind && !has_move && !has_remount && !bx_mount_data_present(options) && options->flag_mentions == 0ul) {
        *print_only = true;
        return true;
    }

    if (!options->explicit_source && !options->explicit_target && remaining == 1 && options->fstype == NULL && !has_bind && !has_move && !has_remount && !bx_mount_data_present(options) && options->flag_mentions == 0ul) {
        *print_only = true;
        *single_target = argv[options->operand_index];
        return true;
    }

    if (has_rec && !has_bind) {
        bx_diag(diag, "recursive mount option requires a bind mount in bx");
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
        return bx_mount_assign_regular_operands(argc, argv, options, diag);
    }

    if (has_bind && !has_remount) {
        if (options->fstype != NULL) {
            bx_diag(diag, "filesystem type is not used with bind mounts");
            return false;
        }
        if (bx_mount_data_present(options)) {
            bx_diag(diag, "bind mounts do not accept filesystem-specific options in bx");
            return false;
        }
        if (options->flag_mentions != 0ul) {
            bx_diag(diag, "bind mounts cannot change mount flags in one step; use a remount");
            return false;
        }
        return bx_mount_assign_regular_operands(argc, argv, options, diag);
    }

    if (has_remount) {
        if (has_bind && has_rec) {
            bx_diag(diag, "recursive bind remount is not supported in bx");
            return false;
        }
        if (has_bind && options->fstype != NULL) {
            bx_diag(diag, "filesystem type is not used with bind remounts");
            return false;
        }
        if (has_bind && bx_mount_data_present(options)) {
            bx_diag(diag, "bind remounts do not accept filesystem-specific options in bx");
            return false;
        }
        return bx_mount_assign_remount_operands(argc, argv, options, diag);
    }

    if (options->fstype == NULL) {
        bx_diag(diag, "filesystem type required for regular mounts; use -t TYPE");
        return false;
    }

    return bx_mount_assign_regular_operands(argc, argv, options, diag);
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

static void bx_mount_success_message(const struct bx_mount_options* options) {
    if (!options->verbose) {
        return;
    }

    if (bx_mount_has_move(options)) {
        printf("%s: %s moved to %s.\n", options->progname, options->source, options->target);
    }
    else if (bx_mount_has_bind(options)) {
        printf("%s: %s bound on %s.\n", options->progname, options->source, options->target);
    }
    else {
        printf("%s: %s mounted on %s.\n", options->progname, options->source, options->target);
    }
}

static bool bx_mount_perform(const struct bx_mount_options* options, struct bx_diag_ctx* diag) {
    char* resolved_source = NULL;
    const char* source = options->source;
    const char* fstype = options->fstype;
    const void* data = bx_mount_data_present(options) ? options->data : NULL;
    unsigned long flags = options->op_flags | options->flag_values;

    if (source != NULL) {
        resolved_source = bx_mount_resolve_source_spec(source);
        if (resolved_source == NULL && (strncmp(source, "LABEL=", 6) == 0 || strncmp(source, "UUID=", 5) == 0 || strncmp(source, "PARTLABEL=", 10) == 0 || strncmp(source, "PARTUUID=", 9) == 0)) {
            bx_diag(diag, "cannot resolve source '%s': %s", source, strerror(errno));
            return false;
        }
        if (resolved_source != NULL) {
            source = resolved_source;
        }
    }

    if (bx_mount_has_bind(options) || bx_mount_has_move(options)) {
        fstype = NULL;
        data = NULL;
    }

    if (mount(source, options->target, fstype, flags, data) != 0) {
        free(resolved_source);
        bx_mount_diag_failure(options, diag);
        return false;
    }

    free(resolved_source);
    bx_mount_success_message(options);
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
    bool print_only = false;
    const char* single_target = NULL;

    if (!bx_mount_parse_options(argc, argv, &options, &diag)) {
        free(options.owned_source);
        free(options.data);
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_mount_print_help(stdout, options.progname);
        free(options.owned_source);
        free(options.data);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        free(options.owned_source);
        free(options.data);
        return 0;
    }

    if (!bx_mount_validate_request(argc, argv, &options, &diag, &print_only, &single_target)) {
        free(options.owned_source);
        free(options.data);
        bx_cli_print_try_help(options.progname);
        return 1;
    }

    if (print_only) {
        int rc = bx_mount_print_mounts(&options, single_target, &diag);
        free(options.owned_source);
        free(options.data);
        return rc;
    }

    if (!bx_mount_perform(&options, &diag)) {
        free(options.owned_source);
        free(options.data);
        return (diag.exit_status != 0) ? diag.exit_status : 1;
    }

    free(options.owned_source);
    free(options.data);
    return 0;
}
