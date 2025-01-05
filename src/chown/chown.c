#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets.h"
#include "common/path_ops.h"
#include "diag.h"
#include "libbx.h"

enum bx_chown_report_mode {
    BX_CHOWN_REPORT_NONE = 0,
    BX_CHOWN_REPORT_CHANGES,
    BX_CHOWN_REPORT_VERBOSE,
};

struct bx_chown_owner_group {
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
};

enum bx_chown_symlink_traversal {
    BX_CHOWN_SYMLINK_TRAVERSAL_P = 0,
    BX_CHOWN_SYMLINK_TRAVERSAL_H,
    BX_CHOWN_SYMLINK_TRAVERSAL_L,
};

struct bx_chown_options {
    const char* progname;
    bool recursive;
    bool no_dereference;
    bool quiet;
    enum bx_chown_report_mode report_mode;
    enum bx_chown_symlink_traversal symlink_traversal;
    const char* reference_path;
    bool from_filter_set;
    struct bx_chown_owner_group from_owner_group;
    bool show_help;
    bool show_version;
};

static bool bx_chown_parse_owner_group(const char* text, struct bx_chown_owner_group* parsed, struct bx_diag_ctx* diag);

static const char* bx_chown_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "chown";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_chown_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [OWNER][:[GROUP]] FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... --reference=RFILE FILE...\n", progname);
    fprintf(stream, "Change the owner and/or group of each FILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes    like verbose but report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet  suppress most error messages\n");
    fprintf(stream, "  -h, --no-dereference   affect symbolic links instead of referenced files\n");
    fprintf(stream, "  -H                     follow command-line symbolic links during recursion\n");
    fprintf(stream, "  -L                     follow all symbolic links during recursion\n");
    fprintf(stream, "  -P                     do not follow symbolic links during recursion (default)\n");
    fprintf(stream, "  -R, --recursive        operate on files and directories recursively\n");
    fprintf(stream, "      --from=CURRENT_OWNER:CURRENT_GROUP\n");
    fprintf(stream, "                         change only when the current owner/group match\n");
    fprintf(stream, "      --reference=RFILE  use RFILE's owner and group instead of OWNER:GROUP values\n");
    fprintf(stream, "  -v, --verbose    output a diagnostic for every file processed\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
}

static void bx_chown_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_chown_parse_options(int argc, char** argv, struct bx_chown_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"changes", no_argument, NULL, 'c'},        {"recursive", no_argument, NULL, 'R'}, {"quiet", no_argument, NULL, 'f'},    {"silent", no_argument, NULL, 'f'},
        {"no-dereference", no_argument, NULL, 'h'}, {"dereference", no_argument, NULL, 5}, {"from", required_argument, NULL, 4}, {"reference", required_argument, NULL, 3},
        {"verbose", no_argument, NULL, 'v'},        {"help", no_argument, NULL, 1},        {"version", no_argument, NULL, 2},    {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_chown_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:RcfhHLPv", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'R':
                options->recursive = true;
                break;
            case 'c':
                options->report_mode = BX_CHOWN_REPORT_CHANGES;
                break;
            case 'f':
                options->quiet = true;
                break;
            case 'h':
                options->no_dereference = true;
                break;
            case 'H':
                options->symlink_traversal = BX_CHOWN_SYMLINK_TRAVERSAL_H;
                break;
            case 'L':
                options->symlink_traversal = BX_CHOWN_SYMLINK_TRAVERSAL_L;
                break;
            case 'P':
                options->symlink_traversal = BX_CHOWN_SYMLINK_TRAVERSAL_P;
                break;
            case 3:
                options->reference_path = optarg;
                break;
            case 4:
                if (!bx_chown_parse_owner_group(optarg, &options->from_owner_group, diag)) {
                    return false;
                }
                options->from_filter_set = true;
                break;
            case 5:
                options->no_dereference = false;
                break;
            case 'v':
                options->report_mode = BX_CHOWN_REPORT_VERBOSE;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case ':':
                if (optopt != 0) {
                    bx_diag(diag, "option requires an argument -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
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

    *first_operand = optind;
    return true;
}

static void bx_chown_perror_path(const struct bx_chown_options* options, struct bx_diag_ctx* diag, const char* path) {
    if (options->quiet) {
        diag->exit_status = 1;
        return;
    }

    bx_perror_path(diag, path);
}

static bool bx_chown_emit_report(const struct bx_chown_options* options, const char* path, uid_t old_owner, gid_t old_group, uid_t new_owner, gid_t new_group, bool changed, struct bx_diag_ctx* diag) {
    if (options->report_mode == BX_CHOWN_REPORT_NONE) {
        return true;
    }
    if (options->report_mode == BX_CHOWN_REPORT_CHANGES && !changed) {
        return true;
    }

    int wrote = 0;
    if (changed) {
        wrote = fprintf(stdout, "ownership of '%s' changed from %" PRIuMAX ":%" PRIuMAX " to %" PRIuMAX ":%" PRIuMAX "\n", path, (uintmax_t)old_owner, (uintmax_t)old_group, (uintmax_t)new_owner,
                        (uintmax_t)new_group);
    }
    else {
        wrote = fprintf(stdout, "ownership of '%s' retained as %" PRIuMAX ":%" PRIuMAX "\n", path, (uintmax_t)new_owner, (uintmax_t)new_group);
    }

    if (wrote < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_chown_parse_id_numeric(const char* text, uintmax_t max_value, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }
    if ((uintmax_t)value > max_value) {
        return false;
    }

    *value_out = (uintmax_t)value;
    return true;
}

static bool bx_chown_parse_owner(const char* text, uid_t* owner_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_chown_parse_id_numeric(text, (uintmax_t)((uid_t)-1), &numeric_id)) {
        *owner_out = (uid_t)numeric_id;
        return true;
    }

    struct passwd* passwd_entry = getpwnam(text);
    if (passwd_entry != NULL) {
        *owner_out = passwd_entry->pw_uid;
        return true;
    }

    bx_diag(diag, "invalid user '%s'", (text != NULL) ? text : "");
    return false;
}

static bool bx_chown_parse_group(const char* text, gid_t* group_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_chown_parse_id_numeric(text, (uintmax_t)((gid_t)-1), &numeric_id)) {
        *group_out = (gid_t)numeric_id;
        return true;
    }

    struct group* group_entry = getgrnam(text);
    if (group_entry != NULL) {
        *group_out = group_entry->gr_gid;
        return true;
    }

    bx_diag(diag, "invalid group '%s'", (text != NULL) ? text : "");
    return false;
}

static bool bx_chown_parse_owner_group(const char* text, struct bx_chown_owner_group* parsed, struct bx_diag_ctx* diag) {
    memset(parsed, 0, sizeof(*parsed));

    if (text == NULL) {
        bx_diag(diag, "missing operand");
        return false;
    }

    char* spec = xstrdup(text);
    char* separator = strchr(spec, ':');
    if (separator == NULL) {
        if (spec[0] != '\0') {
            if (!bx_chown_parse_owner(spec, &parsed->owner, diag)) {
                free(spec);
                return false;
            }
            parsed->owner_set = true;
        }

        free(spec);
        return true;
    }

    *separator = '\0';
    const char* owner_text = spec;
    const char* group_text = separator + 1;

    if (owner_text[0] != '\0') {
        if (!bx_chown_parse_owner(owner_text, &parsed->owner, diag)) {
            free(spec);
            return false;
        }
        parsed->owner_set = true;
    }
    if (group_text[0] != '\0') {
        if (!bx_chown_parse_group(group_text, &parsed->group, diag)) {
            free(spec);
            return false;
        }
        parsed->group_set = true;
    }

    free(spec);
    return true;
}

static bool bx_chown_parse_reference_owner_group(const char* reference_path, struct bx_chown_owner_group* parsed, struct bx_diag_ctx* diag) {
    struct stat st;
    if (stat(reference_path, &st) != 0) {
        bx_perror_path(diag, reference_path);
        return false;
    }

    memset(parsed, 0, sizeof(*parsed));
    parsed->owner_set = true;
    parsed->group_set = true;
    parsed->owner = st.st_uid;
    parsed->group = st.st_gid;
    return true;
}

static bool bx_chown_matches_from_filter(const struct bx_chown_options* options, uid_t current_owner, gid_t current_group) {
    if (!options->from_filter_set) {
        return true;
    }
    if (options->from_owner_group.owner_set && current_owner != options->from_owner_group.owner) {
        return false;
    }
    if (options->from_owner_group.group_set && current_group != options->from_owner_group.group) {
        return false;
    }
    return true;
}

static bool bx_chown_should_follow_symlink_for_apply(const struct bx_chown_options* options, bool top_level) {
    if (options->no_dereference) {
        return false;
    }
    if (!options->recursive) {
        return true;
    }

    if (top_level) {
        return options->symlink_traversal != BX_CHOWN_SYMLINK_TRAVERSAL_P;
    }
    return options->symlink_traversal == BX_CHOWN_SYMLINK_TRAVERSAL_L;
}

static bool bx_chown_should_follow_symlink_for_recursion(const struct bx_chown_options* options, bool top_level) {
    if (!options->recursive) {
        return false;
    }
    if (top_level) {
        return options->symlink_traversal != BX_CHOWN_SYMLINK_TRAVERSAL_P;
    }
    return options->symlink_traversal == BX_CHOWN_SYMLINK_TRAVERSAL_L;
}

static bool bx_chown_apply_existing(const char* path, const struct stat* st, bool no_follow, uid_t owner, gid_t group, const struct bx_chown_options* options, struct bx_diag_ctx* diag) {
    uid_t old_owner = st->st_uid;
    gid_t old_group = st->st_gid;
    uid_t new_owner = owner;
    gid_t new_group = group;
    bool should_apply = true;

    if (owner == (uid_t)-1) {
        new_owner = old_owner;
    }
    if (group == (gid_t)-1) {
        new_group = old_group;
    }

    if (options->from_filter_set) {
        should_apply = bx_chown_matches_from_filter(options, old_owner, old_group);
    }

    if (should_apply) {
        int rc = no_follow ? lchown(path, owner, group) : chown(path, owner, group);
        if (rc != 0) {
            bx_chown_perror_path(options, diag, path);
            return false;
        }
    }
    else {
        new_owner = old_owner;
        new_group = old_group;
    }

    if (options->report_mode != BX_CHOWN_REPORT_NONE) {
        bool changed = should_apply && ((old_owner != new_owner) || (old_group != new_group));
        return bx_chown_emit_report(options, path, old_owner, old_group, new_owner, new_group, changed, diag);
    }

    return true;
}

static bool bx_chown_apply_path_recursive(const char* path, bool top_level, uid_t owner, gid_t group, const struct bx_chown_options* options, struct bx_diag_ctx* diag) {
    struct stat path_lstat;
    if (lstat(path, &path_lstat) != 0) {
        bx_chown_perror_path(options, diag, path);
        return false;
    }

    bool is_symlink = S_ISLNK(path_lstat.st_mode);
    bool follow_for_apply = bx_chown_should_follow_symlink_for_apply(options, top_level);
    bool follow_for_recursion = is_symlink && bx_chown_should_follow_symlink_for_recursion(options, top_level);
    bool no_follow = is_symlink && !follow_for_apply;

    struct stat target_stat;
    struct stat apply_stat = path_lstat;
    bool have_target_stat = false;
    if (is_symlink && (follow_for_apply || follow_for_recursion)) {
        if (stat(path, &target_stat) != 0) {
            bx_chown_perror_path(options, diag, path);
            return false;
        }
        have_target_stat = true;
        if (follow_for_apply) {
            apply_stat = target_stat;
        }
    }

    bool ok = bx_chown_apply_existing(path, &apply_stat, no_follow, owner, group, options, diag);

    bool should_recurse = false;
    if (!options->recursive) {
        should_recurse = false;
    }
    else if (is_symlink) {
        should_recurse = follow_for_recursion && have_target_stat && S_ISDIR(target_stat.st_mode);
    }
    else {
        should_recurse = S_ISDIR(path_lstat.st_mode);
    }

    if (!should_recurse) {
        return ok;
    }

    DIR* dir = opendir(path);
    if (dir == NULL) {
        bx_chown_perror_path(options, diag, path);
        return false;
    }

    bool recurse_ok = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) {
                bx_chown_perror_path(options, diag, path);
                recurse_ok = false;
            }
            break;
        }
        if (bx_path_is_dot_or_dotdot(entry->d_name)) {
            continue;
        }

        char* child_path = bx_path_join(path, entry->d_name);
        if (!bx_chown_apply_path_recursive(child_path, false, owner, group, options, diag)) {
            recurse_ok = false;
        }
        free(child_path);
    }

    if (closedir(dir) != 0) {
        bx_chown_perror_path(options, diag, path);
        recurse_ok = false;
    }

    return ok && recurse_ok;
}

static bool bx_chown_apply_one(const char* path, uid_t owner, gid_t group, const struct bx_chown_options* options, struct bx_diag_ctx* diag) {
    return bx_chown_apply_path_recursive(path, true, owner, group, options, diag);
}

int bx_chown_main(int argc, char** argv) {
    struct bx_chown_options options;
    struct bx_diag_ctx diag = {
        .progname = "chown",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_chown_parse_options(argc, argv, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_chown_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_chown_print_version(options.progname);
        return 0;
    }

    struct bx_chown_owner_group owner_group;
    int operand_count = argc - first_operand;
    int file_start = first_operand;

    if (options.reference_path != NULL) {
        if (!bx_chown_parse_reference_owner_group(options.reference_path, &owner_group, &diag)) {
            return diag.exit_status;
        }

        if (operand_count <= 0) {
            bx_diag(&diag, "missing operand");
            return diag.exit_status;
        }
    }
    else {
        if (operand_count <= 0) {
            bx_diag(&diag, "missing operand");
            return diag.exit_status;
        }
        if (operand_count <= 1) {
            bx_diag(&diag, "missing operand after '%s'", argv[first_operand]);
            return diag.exit_status;
        }

        if (!bx_chown_parse_owner_group(argv[first_operand], &owner_group, &diag)) {
            return diag.exit_status;
        }

        file_start = first_operand + 1;
    }

    if (file_start < argc && strcmp(argv[file_start], "--") == 0) {
        file_start++;
    }
    if (file_start >= argc) {
        if (options.reference_path != NULL) {
            bx_diag(&diag, "missing operand");
        }
        else {
            bx_diag(&diag, "missing operand after '%s'", argv[first_operand]);
        }
        return diag.exit_status;
    }

    uid_t owner = owner_group.owner_set ? owner_group.owner : (uid_t)-1;
    gid_t group = owner_group.group_set ? owner_group.group : (gid_t)-1;

    for (int i = file_start; i < argc; i++) {
        (void)bx_chown_apply_one(argv[i], owner, group, &options, &diag);
    }

    if (options.report_mode != BX_CHOWN_REPORT_NONE && fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}
