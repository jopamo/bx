#define _DEFAULT_SOURCE

#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/mount_table.h"

#ifndef MNT_FORCE
#define MNT_FORCE 0
#endif

struct bx_umount_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool all;
    bool force;
    bool lazy;
    bool quiet;
    bool graceful;
    bool recursive;
    bool read_only;
    bool verbose;
    const char* types;
    int operand_index;
};

static const char* bx_umount_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "umount";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_umount_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-hV]\n", progname);
    fprintf(stream, "       %s -a [options]\n", progname);
    fprintf(stream, "       %s [options] <source> | <directory>...\n", progname);
    fprintf(stream, "Unmount filesystems.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Supported subset:\n");
    fprintf(stream, "  -a, --all      unmount selected filesystems in reverse order\n");
    fprintf(stream, "  -f, --force    force unmount (filesystem support required)\n");
    fprintf(stream, "  -l, --lazy     detach filesystem now, clean up references later\n");
    fprintf(stream, "  -R, --recursive recursively unmount a target with all children\n");
    fprintf(stream, "  -r, --read-only try remount read-only if unmount fails\n");
    fprintf(stream, "  -t, --types LIST limit -a by filesystem type list\n");
    fprintf(stream, "  -q, --quiet    suppress not-mounted diagnostics\n");
    fprintf(stream, "  -g, --graceful succeed if target is already not mounted\n");
    fprintf(stream, "  -v, --verbose  say what is being done\n");
    fprintf(stream, "  -n, --no-mtab  accepted for compatibility; ignored\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_umount_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static void bx_umount_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static char* bx_umount_trim_token(char* text) {
    while (*text != '\0' && (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r' || *text == '\f' || *text == '\v')) {
        text++;
    }

    size_t len = strlen(text);
    while (len > 0) {
        char ch = text[len - 1];
        if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r' && ch != '\f' && ch != '\v') {
            break;
        }
        text[--len] = '\0';
    }

    return text;
}

static bool bx_umount_type_matches(const char* pattern, const char* type) {
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

        token = bx_umount_trim_token(token);
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

static bool bx_umount_target_is_path_or_child(const char* target, const char* root) {
    size_t root_len;

    if (target == NULL || root == NULL) {
        return false;
    }
    if (strcmp(target, root) == 0) {
        return true;
    }

    root_len = strlen(root);
    if (strncmp(target, root, root_len) != 0) {
        return false;
    }
    return target[root_len] == '/';
}

static bool bx_umount_skip_all_target(const char* target) {
    static const char* const exact_skips[] = {
        "/",
        "/dev",
        "/dev/shm",
        "/run",
        "/sys",
        "/tmp",
        "/var/cache",
        "/var/log",
        "/var/tmp",
    };
    static const char* const subtree_skips[] = {
        "/proc",
        "/run/credentials",
        "/run/host",
        "/run/user",
    };

    if (target == NULL || target[0] == '\0') {
        return true;
    }

    for (size_t i = 0; i < sizeof(exact_skips) / sizeof(exact_skips[0]); i++) {
        if (strcmp(target, exact_skips[i]) == 0) {
            return true;
        }
    }

    for (size_t i = 0; i < sizeof(subtree_skips) / sizeof(subtree_skips[0]); i++) {
        if (bx_umount_target_is_path_or_child(target, subtree_skips[i])) {
            return true;
        }
    }

    return false;
}

static bool bx_umount_skip_all_entry(const struct bx_mount_entry* entry, const struct bx_umount_options* options) {
    static const char* const default_skipped_types[] = {
        "proc",
        "sysfs",
        "devpts",
        "devtmpfs",
        "rpc_pipefs",
        "nfsd",
        "selinuxfs",
        "cgroup",
        "cgroup2",
        "securityfs",
        "debugfs",
        "tracefs",
        "bpf",
        "mqueue",
        "pstore",
        "configfs",
        "fusectl",
        "binfmt_misc",
        "autofs",
        "hugetlbfs",
        "efivarfs",
    };

    if (entry == NULL || entry->target == NULL || entry->target[0] == '\0') {
        return true;
    }
    if (strcmp(entry->target, "/") == 0) {
        return true;
    }

    if (bx_umount_skip_all_target(entry->target)) {
        return true;
    }

    if (options->types != NULL) {
        return !bx_umount_type_matches(options->types, entry->fstype);
    }

    for (size_t i = 0; i < sizeof(default_skipped_types) / sizeof(default_skipped_types[0]); i++) {
        if (entry->fstype != NULL && strcmp(entry->fstype, default_skipped_types[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool bx_umount_parse_options(int argc, char** argv, struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"force", no_argument, NULL, 'f'},
        {"lazy", no_argument, NULL, 'l'},
        {"no-mtab", no_argument, NULL, 'n'},
        {"quiet", no_argument, NULL, 'q'},
        {"graceful", no_argument, NULL, 'g'},
        {"recursive", no_argument, NULL, 'R'},
        {"read-only", no_argument, NULL, 'r'},
        {"types", required_argument, NULL, 't'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_umount_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;
    diag->verbose = false;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+aflnt:qgRrv", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                options->all = true;
                break;
            case 'f':
                options->force = true;
                break;
            case 'l':
                options->lazy = true;
                break;
            case 'n':
                break;
            case 't':
                options->types = optarg;
                break;
            case 'q':
                options->quiet = true;
                break;
            case 'g':
                options->graceful = true;
                break;
            case 'R':
                options->recursive = true;
                break;
            case 'r':
                options->read_only = true;
                break;
            case 'v':
                options->verbose = true;
                diag->verbose = true;
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

    options->operand_index = optind;
    return true;
}

static bool bx_umount_validate_request(int argc, char** argv, struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    int remaining = argc - options->operand_index;

    if (options->types != NULL && !options->all) {
        bx_diag(diag, "--types currently requires --all in bx");
        return false;
    }

    if (options->all) {
        if (remaining != 0) {
            bx_diag(diag, "unexpected operand '%s' with --all", argv[options->operand_index]);
            return false;
        }
        return true;
    }

    if (remaining <= 0) {
        bx_diag(diag, "missing target operand");
        return false;
    }

    for (int i = options->operand_index; i < argc; i++) {
        if (argv[i] == NULL || argv[i][0] == '\0') {
            bx_diag(diag, "target may not be empty");
            return false;
        }
    }

    return true;
}

static void bx_umount_success_message(const struct bx_umount_options* options, const char* target, const char* source) {
    if (!options->verbose) {
        return;
    }

    if (source != NULL && source[0] != '\0') {
        printf("%s: %s (%s) unmounted\n", options->progname, target, source);
    }
    else {
        printf("%s: %s unmounted\n", options->progname, target);
    }
}

static int bx_umount_emit_missing(const struct bx_umount_options* options, struct bx_diag_ctx* diag, const char* target) {
    int exists = access(target, F_OK);

    if (options->graceful) {
        return 0;
    }

    if (!options->quiet) {
        if (exists == 0) {
            bx_diag(diag, "cannot unmount '%s': not mounted", target);
        }
        else {
            bx_diag(diag, "cannot unmount '%s': mount target not found: %s", target, strerror(errno));
        }
    }

    return 1;
}

static int bx_umount_perform_one(const struct bx_umount_options* options, const char* target, const char* source, struct bx_diag_ctx* diag) {
    int flags = 0;
    if (options->force) {
        flags |= MNT_FORCE;
    }
    if (options->lazy) {
        flags |= MNT_DETACH;
    }

    if (umount2(target, flags) == 0) {
        bx_umount_success_message(options, target, source);
        return 0;
    }

    int umount_error = errno;

    if (options->read_only && !options->lazy) {
        if (mount(NULL, target, NULL, MS_REMOUNT | MS_RDONLY, NULL) == 0) {
            if (options->verbose) {
                printf("%s: %s remounted read-only\n", options->progname, target);
            }
            return 0;
        }
        errno = umount_error;
    }

    if (options->graceful && (umount_error == EINVAL || umount_error == ENOENT || umount_error == ENOTDIR)) {
        return 0;
    }

    if (options->quiet && (umount_error == EINVAL || umount_error == ENOENT || umount_error == ENOTDIR)) {
        return 1;
    }

    if (umount_error == EBUSY) {
        bx_diag(diag, "cannot unmount '%s': target is busy: %s", target, strerror(umount_error));
    }
    else if (umount_error == ENOENT || umount_error == ENOTDIR) {
        bx_diag(diag, "cannot unmount '%s': mount target not found: %s", target, strerror(umount_error));
    }
    else if (umount_error == EINVAL) {
        bx_diag(diag, "cannot unmount '%s': not mounted: %s", target, strerror(umount_error));
    }
    else {
        bx_diag(diag, "cannot unmount '%s': %s", target, strerror(umount_error));
    }

    return 1;
}

struct bx_target_list {
    struct bx_target_item* items;
    size_t len;
    size_t cap;
};

struct bx_target_item {
    char* target;
    char* source;
};

static void bx_target_list_free(struct bx_target_list* list) {
    if (list == NULL) {
        return;
    }
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].target);
        free(list->items[i].source);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool bx_target_list_contains(const struct bx_target_list* list, const char* target) {
    for (size_t i = 0; i < list->len; i++) {
        if (strcmp(list->items[i].target, target) == 0) {
            return true;
        }
    }
    return false;
}

static void bx_target_list_push(struct bx_target_list* list, const char* target, const char* source) {
    if (bx_target_list_contains(list, target)) {
        return;
    }
    if (list->len == list->cap) {
        size_t new_cap = list->cap == 0 ? 8 : list->cap * 2;
        list->items = xrealloc(list->items, new_cap * sizeof(*list->items));
        list->cap = new_cap;
    }
    list->items[list->len].target = xstrdup(target);
    list->items[list->len].source = source != NULL ? xstrdup(source) : NULL;
    list->len++;
}

static size_t bx_target_depth(const char* target) {
    size_t depth = 0;

    if (target == NULL) {
        return 0;
    }

    for (const char* p = target; *p != '\0'; p++) {
        if (*p == '/') {
            depth++;
        }
    }

    return depth;
}

static int bx_target_item_compare_desc(const void* left, const void* right) {
    const struct bx_target_item* a = left;
    const struct bx_target_item* b = right;
    size_t a_depth = bx_target_depth(a->target);
    size_t b_depth = bx_target_depth(b->target);

    if (a_depth != b_depth) {
        return a_depth < b_depth ? 1 : -1;
    }

    size_t a_len = strlen(a->target);
    size_t b_len = strlen(b->target);
    if (a_len != b_len) {
        return a_len < b_len ? 1 : -1;
    }

    return strcmp(a->target, b->target);
}

static void bx_target_list_sort_desc(struct bx_target_list* list) {
    if (list == NULL || list->len < 2) {
        return;
    }

    qsort(list->items, list->len, sizeof(*list->items), bx_target_item_compare_desc);
}

static int bx_umount_recursive(const struct bx_umount_options* options, const char* spec, struct bx_diag_ctx* diag) {
    struct bx_mount_table table;
    struct bx_target_list list = {0};

    if (!bx_mount_table_load(&table)) {
        bx_diag(diag, "failed to read /proc/self/mounts: %s", strerror(errno));
        return 1;
    }

    if (bx_mount_table_find_target(&table, spec) == NULL) {
        bx_mount_table_free(&table);
        return bx_umount_emit_missing(options, diag, spec);
    }

    for (size_t i = table.len; i-- > 0;) {
        const struct bx_mount_entry* entry = &table.entries[i];
        if (bx_mount_table_is_target_or_child(spec, entry->target)) {
            bx_target_list_push(&list, entry->target, entry->source);
        }
    }

    bx_mount_table_free(&table);
    bx_target_list_sort_desc(&list);

    for (size_t i = 0; i < list.len; i++) {
        int rc = bx_umount_perform_one(options, list.items[i].target, list.items[i].source, diag);
        if (rc != 0) {
            bx_target_list_free(&list);
            return rc;
        }
    }

    bx_target_list_free(&list);
    return 0;
}

static int bx_umount_all(const struct bx_umount_options* options, struct bx_diag_ctx* diag) {
    struct bx_mount_table table;
    struct bx_target_list list = {0};
    int successes = 0;
    int failures = 0;

    if (!bx_mount_table_load(&table)) {
        bx_diag(diag, "failed to read /proc/self/mounts: %s", strerror(errno));
        return 1;
    }

    for (size_t i = table.len; i-- > 0;) {
        const struct bx_mount_entry* entry = &table.entries[i];
        if (bx_umount_skip_all_entry(entry, options)) {
            continue;
        }
        bx_target_list_push(&list, entry->target, entry->source);
    }

    bx_mount_table_free(&table);
    bx_target_list_sort_desc(&list);

    for (size_t i = 0; i < list.len; i++) {
        int rc = bx_umount_perform_one(options, list.items[i].target, list.items[i].source, diag);
        if (rc == 0) {
            successes++;
        }
        else {
            failures++;
        }
    }

    bx_target_list_free(&list);

    if (failures == 0) {
        return 0;
    }
    if (successes == 0) {
        return 1;
    }
    return 64;
}

int bx_umount_main(int argc, char** argv) {
    struct bx_umount_options options;
    struct bx_diag_ctx diag = {
        .progname = "umount",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_umount_parse_options(argc, argv, &options, &diag)) {
        bx_umount_print_try_help(options.progname);
        return 1;
    }

    if (options.show_help) {
        bx_umount_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_umount_print_version(options.progname);
        return 0;
    }

    if (!bx_umount_validate_request(argc, argv, &options, &diag)) {
        bx_umount_print_try_help(options.progname);
        return 1;
    }

    if (options.all) {
        return bx_umount_all(&options, &diag);
    }

    int successes = 0;
    int failures = 0;

    for (int i = options.operand_index; i < argc; i++) {
        const char* target = argv[i];
        int rc;

        if (options.recursive) {
            rc = bx_umount_recursive(&options, target, &diag);
        }
        else {
            struct bx_mount_table table;
            const struct bx_mount_entry* entry = NULL;
            const char* source = NULL;

            if (!bx_mount_table_load(&table)) {
                bx_diag(&diag, "failed to read /proc/self/mounts: %s", strerror(errno));
                return 1;
            }
            entry = bx_mount_table_find_target(&table, target);
            if (entry == NULL) {
                bx_mount_table_free(&table);
                rc = bx_umount_emit_missing(&options, &diag, target);
            }
            else {
                source = entry->source;
                rc = bx_umount_perform_one(&options, target, source, &diag);
                bx_mount_table_free(&table);
            }
        }

        if (rc == 0) {
            successes++;
        }
        else {
            failures++;
        }
    }

    if (failures == 0) {
        return 0;
    }
    if (successes == 0) {
        return 1;
    }
    return 64;
}
