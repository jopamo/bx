#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>

#include "applets.h"
#include "diag.h"
#include "libbx.h"

struct bx_id_options {
    const char* progname;
    bool only_context;
    bool only_group;
    bool all_groups;
    bool only_name;
    bool only_real;
    bool only_user;
    bool zero_terminated;
    bool show_help;
    bool show_version;
};

static void bx_id_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [USER]...\n", progname);
    fprintf(stream, "Print user and group information for each specified USER,\n");
    fprintf(stream, "or (when USER omitted) for the current process.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a                  ignore (compatibility no-op)\n");
    fprintf(stream, "  -Z, --context       print only security context (SELinux only)\n");
    fprintf(stream, "  -g, --group         print only the effective group ID\n");
    fprintf(stream, "  -G, --groups        print all group IDs\n");
    fprintf(stream, "  -n, --name          print names for -u, -g, -G\n");
    fprintf(stream, "  -r, --real          print real IDs for -u, -g, -G\n");
    fprintf(stream, "  -u, --user          print only the effective user ID\n");
    fprintf(stream, "  -z, --zero          use NUL delimiters (not valid in default format)\n");
    fprintf(stream, "      --help          display this help and exit\n");
    fprintf(stream, "      --version       output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Without any OPTION, print uid/gid/groups summary information.\n");
}

static void bx_id_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_id_parse_options(int argc, char** argv, struct bx_id_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"context", no_argument, NULL, 'Z'}, {"group", no_argument, NULL, 'g'}, {"groups", no_argument, NULL, 'G'}, {"name", no_argument, NULL, 'n'},  {"real", no_argument, NULL, 'r'},
        {"user", no_argument, NULL, 'u'},    {"zero", no_argument, NULL, 'z'},  {"help", no_argument, NULL, 1},     {"version", no_argument, NULL, 2}, {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "id";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "aZgGnruz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                break;  // ignored
            case 'Z':
                options->only_context = true;
                break;
            case 'g':
                options->only_group = true;
                break;
            case 'G':
                options->all_groups = true;
                break;
            case 'n':
                options->only_name = true;
                break;
            case 'r':
                options->only_real = true;
                break;
            case 'u':
                options->only_user = true;
                break;
            case 'z':
                options->zero_terminated = true;
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

    if (options->only_name && !(options->only_user || options->only_group || options->all_groups)) {
        bx_diag(diag, "cannot print only names or real IDs in default format");
        return false;
    }
    if (options->only_real && !(options->only_user || options->only_group || options->all_groups)) {
        bx_diag(diag, "cannot print only names or real IDs in default format");
        return false;
    }
    if (options->zero_terminated && !(options->only_user || options->only_group || options->all_groups || options->only_context)) {
        bx_diag(diag, "option --zero not permitted in default format");
        return false;
    }

    int count = (options->only_user ? 1 : 0) + (options->only_group ? 1 : 0) + (options->all_groups ? 1 : 0) + (options->only_context ? 1 : 0);
    if (count > 1) {
        bx_diag(diag, "cannot print \"only\" of more than one choice");
        return false;
    }

    *first_operand = optind;
    return true;
}

static int bx_collect_groups(const char* username, gid_t primary_gid, gid_t** groups_out, struct bx_diag_ctx* diag) {
    gid_t* raw_groups = NULL;
    int raw_count = 0;

    if (username) {
        int capacity = 32;
        raw_groups = xmalloc((size_t)capacity * sizeof(gid_t));
        raw_count = capacity;
        if (getgrouplist(username, primary_gid, raw_groups, &raw_count) == -1) {
            raw_groups = xrealloc(raw_groups, (size_t)raw_count * sizeof(gid_t));
            if (getgrouplist(username, primary_gid, raw_groups, &raw_count) == -1) {
                free(raw_groups);
                bx_diag(diag, "cannot determine groups for '%s'", username);
                return -1;
            }
        }
    }
    else {
        raw_count = getgroups(0, NULL);
        if (raw_count < 0) {
            bx_diag(diag, "cannot get supplementary groups: %s", strerror(errno));
            return -1;
        }
        raw_groups = xmalloc((size_t)(raw_count + 1) * sizeof(gid_t));
        int fetched = getgroups(raw_count, raw_groups);
        if (fetched < 0) {
            free(raw_groups);
            bx_diag(diag, "cannot get supplementary groups: %s", strerror(errno));
            return -1;
        }
        raw_count = fetched;
    }

    gid_t* ordered_groups = xmalloc((size_t)(raw_count + 1) * sizeof(gid_t));
    int ordered_count = 0;
    ordered_groups[ordered_count++] = primary_gid;

    for (int i = 0; i < raw_count; i++) {
        gid_t gid = raw_groups[i];
        bool seen = false;
        for (int j = 0; j < ordered_count; j++) {
            if (ordered_groups[j] == gid) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ordered_groups[ordered_count++] = gid;
        }
    }

    free(raw_groups);
    *groups_out = ordered_groups;
    return ordered_count;
}

static void print_id_info(const char* username, struct bx_id_options* options, struct bx_diag_ctx* diag) {
    uid_t ruid, euid;
    gid_t rgid, egid;
    struct passwd* pwd = NULL;

    if (username) {
        pwd = getpwnam(username);
        if (!pwd) {
            bx_diag(diag, "%s: no such user", username);
            return;
        }
        ruid = euid = pwd->pw_uid;
        rgid = egid = pwd->pw_gid;
    }
    else {
        ruid = getuid();
        euid = geteuid();
        rgid = getgid();
        egid = getegid();
    }

    if (options->only_user) {
        uid_t uid = options->only_real ? ruid : euid;
        if (options->only_name) {
            struct passwd* p = getpwuid(uid);
            if (p)
                printf("%s", p->pw_name);
            else
                printf("%u", (unsigned int)uid);
        }
        else {
            printf("%u", (unsigned int)uid);
        }
        putchar(options->zero_terminated ? '\0' : '\n');
        return;
    }

    if (options->only_group) {
        gid_t gid = options->only_real ? rgid : egid;
        if (options->only_name) {
            struct group* g = getgrgid(gid);
            if (g)
                printf("%s", g->gr_name);
            else
                printf("%u", (unsigned int)gid);
        }
        else {
            printf("%u", (unsigned int)gid);
        }
        putchar(options->zero_terminated ? '\0' : '\n');
        return;
    }

    if (options->all_groups) {
        gid_t primary_gid = options->only_real ? rgid : egid;
        gid_t* groups = NULL;
        int ngroups = bx_collect_groups(username, primary_gid, &groups, diag);
        if (ngroups < 0)
            return;

        for (int i = 0; i < ngroups; i++) {
            if (i > 0)
                putchar(options->zero_terminated ? '\0' : ' ');
            if (options->only_name) {
                struct group* g = getgrgid(groups[i]);
                if (g)
                    printf("%s", g->gr_name);
                else
                    printf("%u", (unsigned int)groups[i]);
            }
            else {
                printf("%u", (unsigned int)groups[i]);
            }
        }
        putchar(options->zero_terminated ? '\0' : '\n');
        free(groups);
        return;
    }

    // Default format: uid=... gid=... groups=...
    struct passwd* rpwd = getpwuid(ruid);
    struct group* rgp = getgrgid(rgid);

    printf("uid=%u", (unsigned int)ruid);
    if (rpwd)
        printf("(%s)", rpwd->pw_name);

    printf(" gid=%u", (unsigned int)rgid);
    if (rgp)
        printf("(%s)", rgp->gr_name);

    if (euid != ruid) {
        struct passwd* upwd = getpwuid(euid);
        printf(" euid=%u", (unsigned int)euid);
        if (upwd)
            printf("(%s)", upwd->pw_name);
    }
    if (egid != rgid) {
        struct group* egp = getgrgid(egid);
        printf(" egid=%u", (unsigned int)egid);
        if (egp)
            printf("(%s)", egp->gr_name);
    }

    printf(" groups=");
    gid_t* groups = NULL;
    int ngroups = 0;
    if (username) {
        ngroups = 32;
        groups = xmalloc((size_t)ngroups * sizeof(gid_t));
        if (getgrouplist(username, egid, groups, &ngroups) == -1) {
            groups = xrealloc(groups, (size_t)ngroups * sizeof(gid_t));
            if (getgrouplist(username, egid, groups, &ngroups) == -1) {
                free(groups);
                bx_diag(diag, "cannot determine groups for '%s'", username);
                return;
            }
        }
    }
    else {
        ngroups = bx_collect_groups(username, egid, &groups, diag);
        if (ngroups < 0)
            return;
    }

    for (int i = 0; i < ngroups; i++) {
        if (i > 0)
            putchar(',');
        struct group* g = getgrgid(groups[i]);
        printf("%u", (unsigned int)groups[i]);
        if (g)
            printf("(%s)", g->gr_name);
    }
    printf("\n");
    free(groups);
}

int bx_id_main(int argc, char** argv) {
    struct bx_id_options options;
    struct bx_diag_ctx diag = {.progname = "id", .exit_status = 0};
    int first_operand = 0;

    if (!bx_id_parse_options(argc, argv, &options, &first_operand, &diag))
        return 1;
    if (options.show_help) {
        bx_id_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_id_print_version(options.progname);
        return 0;
    }

    if (options.only_context) {
        bx_diag(&diag, "--context (-Z) works only on an SELinux-enabled kernel");
        return diag.exit_status;
    }

    int num_users = argc - first_operand;
    if (num_users == 0) {
        print_id_info(NULL, &options, &diag);
    }
    else {
        for (int i = 0; i < num_users; i++) {
            print_id_info(argv[first_operand + i], &options, &diag);
        }
    }

    return diag.exit_status;
}
