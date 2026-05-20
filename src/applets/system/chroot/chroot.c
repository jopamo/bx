#define _DEFAULT_SOURCE

#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

struct bx_chroot_name_or_id {
    char* raw;
};

struct bx_chroot_userspec_arg {
    bool present;
    bool user_present;
    struct bx_chroot_name_or_id user;
    struct bx_chroot_name_or_id group;
    bool group_present;
};

struct bx_chroot_groups_arg {
    bool present;
    char* raw;
    char** items;
    size_t count;
};

struct bx_chroot_cred_spec {
    struct bx_chroot_userspec_arg userspec;
    struct bx_chroot_groups_arg groups;
};

struct bx_chroot_resolved_creds {
    bool uid_set;
    bool gid_set;
    bool groups_set;
    uid_t uid;
    gid_t gid;
    gid_t* groups;
    size_t ngroups;
};

struct bx_chroot_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool skip_chdir;
    const char* newroot;
    int command_index;
    struct bx_chroot_cred_spec cred_spec;
};

static void bx_chroot_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NEWROOT [COMMAND [ARG]...]\n", progname);
    fprintf(stream, "Run COMMAND with root directory set to NEWROOT.\n");
    fprintf(stream, "\n");
    fprintf(stream, "      --userspec=USER[:GROUP]  specify user and group (name or numeric ID)\n");
    fprintf(stream, "      --groups=G_LIST          specify supplementary groups as g1,g2,...,gN\n");
    fprintf(stream, "      --skip-chdir             do not change working directory to '/'\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "If no command is given, run '$SHELL -i' (default: '/bin/sh -i').\n");
}

static void bx_chroot_free_userspec_arg(struct bx_chroot_userspec_arg* userspec) {
    if (userspec == NULL) {
        return;
    }

    free(userspec->user.raw);
    free(userspec->group.raw);
    memset(userspec, 0, sizeof(*userspec));
}

static void bx_chroot_free_groups_arg(struct bx_chroot_groups_arg* groups) {
    if (groups == NULL) {
        return;
    }

    for (size_t i = 0; i < groups->count; i++) {
        free(groups->items[i]);
    }

    free(groups->items);
    free(groups->raw);
    memset(groups, 0, sizeof(*groups));
}

static void bx_chroot_free_cred_spec(struct bx_chroot_cred_spec* spec) {
    if (spec == NULL) {
        return;
    }

    bx_chroot_free_userspec_arg(&spec->userspec);
    bx_chroot_free_groups_arg(&spec->groups);
}

static void bx_chroot_free_resolved_creds(struct bx_chroot_resolved_creds* creds) {
    if (creds == NULL) {
        return;
    }

    free(creds->groups);
    memset(creds, 0, sizeof(*creds));
}

static bool bx_chroot_identity_options_present(const struct bx_chroot_cred_spec* spec) {
    return spec->userspec.present || spec->groups.present;
}

static bool bx_chroot_parse_userspec(const char* text, struct bx_chroot_userspec_arg* out, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "missing operand");
        return false;
    }

    struct bx_chroot_userspec_arg parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.present = true;

    char* spec = xstrdup(text);
    char* separator = strchr(spec, ':');
    if (separator != NULL) {
        *separator = '\0';
        separator++;

        if (separator[0] != '\0') {
            parsed.group.raw = xstrdup(separator);
            parsed.group_present = true;
        }
    }

    if (spec[0] != '\0') {
        parsed.user.raw = xstrdup(spec);
        parsed.user_present = true;
    }

    free(spec);

    bx_chroot_free_userspec_arg(out);
    *out = parsed;
    return true;
}

static bool bx_chroot_parse_groups(const char* text, struct bx_chroot_groups_arg* out, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "missing operand");
        return false;
    }

    struct bx_chroot_groups_arg parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.present = true;
    parsed.raw = xstrdup(text);

    if (text[0] != '\0') {
        char* copy = xstrdup(text);
        char* token = copy;

        while (true) {
            char* comma = strchr(token, ',');
            if (comma != NULL) {
                *comma = '\0';
            }

            if (token[0] != '\0') {
                parsed.items = xrealloc(parsed.items, (parsed.count + 1) * sizeof(*parsed.items));
                parsed.items[parsed.count++] = xstrdup(token);
            }

            if (comma == NULL) {
                break;
            }
            token = comma + 1;
        }

        free(copy);
    }

    bx_chroot_free_groups_arg(out);
    *out = parsed;
    return true;
}

static bool bx_chroot_parse_numeric_id(const char* text, uintmax_t max_value, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }
    if (value > max_value) {
        return false;
    }

    *value_out = value;
    return true;
}

static bool bx_chroot_parse_uid_numeric(const char* text, uid_t* uid_out) {
    uintmax_t value = 0;
    if (!bx_chroot_parse_numeric_id(text, (uintmax_t)((uid_t)-1), &value)) {
        return false;
    }

    *uid_out = (uid_t)value;
    return true;
}

static bool bx_chroot_parse_gid_numeric(const char* text, gid_t* gid_out) {
    uintmax_t value = 0;
    if (!bx_chroot_parse_numeric_id(text, (uintmax_t)((gid_t)-1), &value)) {
        return false;
    }

    *gid_out = (gid_t)value;
    return true;
}

static bool bx_chroot_resolve_user_item(const struct bx_chroot_name_or_id* item, bool need_passwd_entry, uid_t* uid_out, gid_t* primary_gid_out, char** user_name_out, struct bx_diag_ctx* diag) {
    uid_t uid = 0;
    bool is_numeric = bx_chroot_parse_uid_numeric(item->raw, &uid);

    if (is_numeric) {
        *uid_out = uid;
        if (!need_passwd_entry) {
            return true;
        }

        struct passwd* passwd_entry = getpwuid(uid);
        if (passwd_entry == NULL) {
            if (diag != NULL) {
                bx_diag(diag, "invalid user '%s'", item->raw);
            }
            return false;
        }

        if (primary_gid_out != NULL) {
            *primary_gid_out = passwd_entry->pw_gid;
        }
        if (user_name_out != NULL) {
            *user_name_out = xstrdup(passwd_entry->pw_name);
        }
        return true;
    }

    struct passwd* passwd_entry = getpwnam(item->raw);
    if (passwd_entry == NULL) {
        if (diag != NULL) {
            bx_diag(diag, "invalid user '%s'", item->raw);
        }
        return false;
    }

    *uid_out = passwd_entry->pw_uid;
    if (primary_gid_out != NULL) {
        *primary_gid_out = passwd_entry->pw_gid;
    }
    if (user_name_out != NULL) {
        *user_name_out = xstrdup(passwd_entry->pw_name);
    }
    return true;
}

static bool bx_chroot_resolve_group_item(const struct bx_chroot_name_or_id* item, gid_t* gid_out, struct bx_diag_ctx* diag) {
    gid_t gid = 0;
    if (bx_chroot_parse_gid_numeric(item->raw, &gid)) {
        *gid_out = gid;
        return true;
    }

    struct group* group_entry = getgrnam(item->raw);
    if (group_entry == NULL) {
        if (diag != NULL) {
            bx_diag(diag, "invalid group '%s'", item->raw);
        }
        return false;
    }

    *gid_out = group_entry->gr_gid;
    return true;
}

static bool bx_chroot_resolve_supplementary_groups_from_user(const char* user, gid_t primary_gid, gid_t** groups_out, size_t* ngroups_out, struct bx_diag_ctx* diag) {
    *groups_out = NULL;
    *ngroups_out = 0;

    int ngroups = 8;
    gid_t* groups = xmalloc((size_t)ngroups * sizeof(*groups));
    int rc = getgrouplist(user, primary_gid, groups, &ngroups);
    if (rc < 0) {
        free(groups);
        groups = NULL;

        if (ngroups < 0) {
            if (diag != NULL) {
                bx_diag(diag, "failed to resolve supplementary groups for '%s'", user);
            }
            return false;
        }

        if (ngroups > 0) {
            groups = xmalloc((size_t)ngroups * sizeof(*groups));
        }

        int retry_ngroups = ngroups;
        rc = getgrouplist(user, primary_gid, groups, &retry_ngroups);
        if (rc < 0 || retry_ngroups < 0) {
            free(groups);
            if (diag != NULL) {
                bx_diag(diag, "failed to resolve supplementary groups for '%s'", user);
            }
            return false;
        }
        ngroups = retry_ngroups;
    }

    if (ngroups == 0) {
        free(groups);
        groups = NULL;
    }

    *groups_out = groups;
    *ngroups_out = (size_t)ngroups;
    return true;
}

static bool bx_chroot_resolve_explicit_groups(const struct bx_chroot_groups_arg* arg, gid_t** groups_out, size_t* ngroups_out, struct bx_diag_ctx* diag) {
    *groups_out = NULL;
    *ngroups_out = 0;

    if (!arg->present) {
        return true;
    }

    if (arg->count == 0) {
        if (arg->raw != NULL && arg->raw[0] != '\0') {
            bx_diag(diag, "invalid group list '%s'", arg->raw);
            return false;
        }
        return true;
    }

    gid_t* gids = xmalloc(arg->count * sizeof(*gids));
    for (size_t i = 0; i < arg->count; i++) {
        struct bx_chroot_name_or_id item = {
            .raw = arg->items[i],
        };
        if (!bx_chroot_resolve_group_item(&item, &gids[i], diag)) {
            free(gids);
            return false;
        }
    }

    *groups_out = gids;
    *ngroups_out = arg->count;
    return true;
}

static bool bx_chroot_resolve_creds_here(const struct bx_chroot_cred_spec* spec, struct bx_chroot_resolved_creds* out, struct bx_diag_ctx* diag) {
    memset(out, 0, sizeof(*out));

    if (!bx_chroot_identity_options_present(spec)) {
        return true;
    }

    char* supplementary_user = NULL;
    if (spec->userspec.present) {
        if (spec->userspec.user_present) {
            uid_t uid = 0;
            gid_t primary_gid = 0;
            bool needs_passwd_entry = !spec->userspec.group_present || !spec->groups.present;
            bool need_supplementary_groups = !spec->groups.present;

            if (!bx_chroot_resolve_user_item(&spec->userspec.user, needs_passwd_entry, &uid, &primary_gid, need_supplementary_groups ? &supplementary_user : NULL, diag)) {
                goto fail;
            }

            out->uid_set = true;
            out->uid = uid;

            if (spec->userspec.group_present) {
                gid_t gid = 0;
                if (!bx_chroot_resolve_group_item(&spec->userspec.group, &gid, diag)) {
                    goto fail;
                }
                out->gid_set = true;
                out->gid = gid;
            }
            else {
                out->gid_set = true;
                out->gid = primary_gid;
            }

            if (!spec->groups.present) {
                if (!bx_chroot_resolve_supplementary_groups_from_user(supplementary_user, out->gid, &out->groups, &out->ngroups, diag)) {
                    goto fail;
                }
                out->groups_set = true;
            }
        }
        else if (spec->userspec.group_present) {
            gid_t gid = 0;
            if (!bx_chroot_resolve_group_item(&spec->userspec.group, &gid, diag)) {
                goto fail;
            }
            out->gid_set = true;
            out->gid = gid;
        }
    }

    if (spec->groups.present) {
        if (!bx_chroot_resolve_explicit_groups(&spec->groups, &out->groups, &out->ngroups, diag)) {
            goto fail;
        }
        out->groups_set = true;
    }

    free(supplementary_user);
    return true;

fail:
    free(supplementary_user);
    bx_chroot_free_resolved_creds(out);
    return false;
}

static bool bx_chroot_apply_creds(const struct bx_chroot_resolved_creds* creds, struct bx_diag_ctx* diag) {
    if (creds->groups_set) {
        if (setgroups(creds->ngroups, creds->groups) != 0) {
            bx_diag(diag, "failed to set supplemental groups: %s", strerror(errno));
            return false;
        }
    }

    if (creds->gid_set) {
        if (setgid(creds->gid) != 0) {
            bx_diag(diag, "failed to set group ID to %" PRIuMAX ": %s", (uintmax_t)creds->gid, strerror(errno));
            return false;
        }
    }

    if (creds->uid_set) {
        if (setuid(creds->uid) != 0) {
            bx_diag(diag, "failed to set user ID to %" PRIuMAX ": %s", (uintmax_t)creds->uid, strerror(errno));
            return false;
        }
    }

    return true;
}

static bool bx_chroot_newroot_is_old_root(const char* newroot) {
    if (newroot == NULL) {
        return false;
    }

    struct stat old_root_stat;
    struct stat new_root_stat;
    if (stat("/", &old_root_stat) != 0) {
        return false;
    }
    if (stat(newroot, &new_root_stat) != 0) {
        return false;
    }

    return old_root_stat.st_dev == new_root_stat.st_dev && old_root_stat.st_ino == new_root_stat.st_ino;
}

static bool bx_chroot_parse_options(int argc, char** argv, struct bx_chroot_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"userspec", required_argument, NULL, 3}, {"groups", required_argument, NULL, 4}, {"skip-chdir", no_argument, NULL, 5},
        {"help", no_argument, NULL, 1},           {"version", no_argument, NULL, 2},      {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "chroot");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 3:
                if (!bx_chroot_parse_userspec(optarg, &options->cred_spec.userspec, diag)) {
                    return false;
                }
                break;
            case 4:
                if (!bx_chroot_parse_groups(optarg, &options->cred_spec.groups, diag)) {
                    return false;
                }
                break;
            case 5:
                options->skip_chdir = true;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
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

    if (optind >= argc) {
        bx_cli_diag_missing_operand(diag);
        return false;
    }

    options->newroot = argv[optind];
    options->command_index = optind + 1;
    return true;
}

static char** bx_chroot_resolve_command_argv(int argc, char** argv, int command_index, char* default_command_argv[3], char** default_shell_copy_out) {
    *default_shell_copy_out = NULL;

    if (command_index < argc) {
        return argv + command_index;
    }

    const char* shell = getenv("SHELL");
    if (shell == NULL || shell[0] == '\0') {
        shell = "/bin/sh";
    }

    char* shell_copy = xstrdup(shell);
    *default_shell_copy_out = shell_copy;

    default_command_argv[0] = shell_copy;
    default_command_argv[1] = "-i";
    default_command_argv[2] = NULL;
    return default_command_argv;
}

static int bx_chroot_exec_command(char** command_argv, struct bx_diag_ctx* diag) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "failed to run command '%s': %s", command_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_chroot_main(int argc, char** argv) {
    struct bx_chroot_options options;
    struct bx_diag_ctx diag = {
        .progname = "chroot",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    struct bx_chroot_resolved_creds creds;
    memset(&creds, 0, sizeof(creds));
    char* default_shell_copy = NULL;
    int rc = 125;

    if (!bx_chroot_parse_options(argc, argv, &options, &diag)) {
        bx_chroot_free_cred_spec(&options.cred_spec);
        bx_cli_print_try_help(options.progname);
        return 125;
    }

    if (options.show_help) {
        bx_chroot_print_help(stdout, options.progname);
        bx_chroot_free_cred_spec(&options.cred_spec);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_chroot_free_cred_spec(&options.cred_spec);
        return 0;
    }

    if (options.skip_chdir && !bx_chroot_newroot_is_old_root(options.newroot)) {
        bx_diag(&diag, "option --skip-chdir only permitted if NEWROOT is old '/'");
        bx_cli_print_try_help(options.progname);
        goto out;
    }

    bool identity_options_present = bx_chroot_identity_options_present(&options.cred_spec);
    if (chroot(options.newroot) != 0) {
        bx_diag(&diag, "cannot change root directory to '%s': %s", options.newroot, strerror(errno));
        goto out;
    }

    if (!options.skip_chdir && chdir("/") != 0) {
        bx_diag(&diag, "cannot change working directory to '/': %s", strerror(errno));
        goto out;
    }

    if (identity_options_present) {
        if (!bx_chroot_resolve_creds_here(&options.cred_spec, &creds, &diag)) {
            goto out;
        }

        if (!bx_chroot_apply_creds(&creds, &diag)) {
            goto out;
        }
    }

    char* default_command_argv[3];
    char** command_argv = bx_chroot_resolve_command_argv(argc, argv, options.command_index, default_command_argv, &default_shell_copy);
    rc = bx_chroot_exec_command(command_argv, &diag);

out:
    free(default_shell_copy);
    bx_chroot_free_resolved_creds(&creds);
    bx_chroot_free_cred_spec(&options.cred_spec);
    return rc;
}
