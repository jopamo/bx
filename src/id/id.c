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
    fprintf(stream, "  -a                      ignore, for compatibility with other versions\n");
    fprintf(stream, "  -g, --group             print only the effective group ID\n");
    fprintf(stream, "  -G, --groups            print all group IDs\n");
    fprintf(stream, "  -n, --name              print a name instead of a number, for -ugG\n");
    fprintf(stream, "  -r, --real              print the real ID instead of the effective ID, with -ugG\n");
    fprintf(stream, "  -u, --user              print only the effective user ID\n");
    fprintf(stream, "  -z, --zero              delimit output with NUL characters, not whitespace\n");
    fprintf(stream, "      --help     display this help and exit\n");
    fprintf(stream, "      --version  output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Without any OPTION, print some identifying information for each USER.\n");
}

static void bx_id_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool bx_id_parse_options(int argc, char** argv, struct bx_id_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"group", no_argument, NULL, 'g'}, {"groups", no_argument, NULL, 'G'}, {"name", no_argument, NULL, 'n'},
        {"real", no_argument, NULL, 'r'},  {"user", no_argument, NULL, 'u'},   {"zero", no_argument, NULL, 'z'},
        {"help", no_argument, NULL, 1},    {"version", no_argument, NULL, 2},  {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = "id";
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "agGnr_uz", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'a':
                break;  // ignored
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
                bx_diag(diag, "invalid option -- '%c'", optopt);
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

    int count = (options->only_user ? 1 : 0) + (options->only_group ? 1 : 0) + (options->all_groups ? 1 : 0);
    if (count > 1) {
        bx_diag(diag, "cannot print \"only\" of more than one choice");
        return false;
    }

    *first_operand = optind;
    return true;
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
        gid_t* groups;
        int ngroups;
        if (username) {
            // Need to find groups for this user
            struct passwd* p = getpwnam(username);
            ngroups = 32;
            groups = xmalloc(ngroups * sizeof(gid_t));
            if (getgrouplist(username, p->pw_gid, groups, &ngroups) == -1) {
                groups = xrealloc(groups, ngroups * sizeof(gid_t));
                getgrouplist(username, p->pw_gid, groups, &ngroups);
            }
        }
        else {
            ngroups = getgroups(0, NULL);
            groups = xmalloc((ngroups + 1) * sizeof(gid_t));
            ngroups = getgroups(ngroups, groups);
            // Also include effective/real group if not already there
            gid_t gid = options->only_real ? rgid : egid;
            bool found = false;
            for (int i = 0; i < ngroups; i++)
                if (groups[i] == gid)
                    found = true;
            if (!found)
                groups[ngroups++] = gid;
        }

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
    struct passwd* upwd = getpwuid(euid);
    struct group* egp = getgrgid(egid);

    printf("uid=%u", (unsigned int)euid);
    if (upwd)
        printf("(%s)", upwd->pw_name);

    printf(" gid=%u", (unsigned int)egid);
    if (egp)
        printf("(%s)", egp->gr_name);

    if (euid != ruid) {
        struct passwd* rpwd = getpwuid(ruid);
        printf(" euid=%u", (unsigned int)ruid);
        if (rpwd)
            printf("(%s)", rpwd->pw_name);
    }
    if (egid != rgid) {
        struct group* rgp = getgrgid(rgid);
        printf(" egid=%u", (unsigned int)rgid);
        if (rgp)
            printf("(%s)", rgp->gr_name);
    }

    printf(" groups=");
    gid_t* groups;
    int ngroups;
    if (username) {
        struct passwd* p = getpwnam(username);
        ngroups = 32;
        groups = xmalloc(ngroups * sizeof(gid_t));
        if (getgrouplist(username, p->pw_gid, groups, &ngroups) == -1) {
            groups = xrealloc(groups, ngroups * sizeof(gid_t));
            getgrouplist(username, p->pw_gid, groups, &ngroups);
        }
    }
    else {
        ngroups = getgroups(0, NULL);
        groups = xmalloc((ngroups + 1) * sizeof(gid_t));
        ngroups = getgroups(ngroups, groups);
        bool found = false;
        for (int i = 0; i < ngroups; i++)
            if (groups[i] == egid)
                found = true;
        if (!found)
            groups[ngroups++] = egid;
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
