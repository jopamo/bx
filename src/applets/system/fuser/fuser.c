#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "applets/system/psmisc/signals.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/id_parse.h"
#include "lib/path_ops.h"
#include "lib/prompt_ops.h"

struct bx_fuser_options {
    int signal_number;
    bool all;
    bool mount;
    bool interactive;
    bool inode;
    bool kill;
    bool list_signals;
    bool ismountpoint;
    bool silent;
    bool user;
    bool verbose;
    bool writeonly;
    bool ipv4;
    bool ipv6;
    const char* namespace_name;
    int first_name_index;
};

struct bx_fuser_target {
    const char* name;
    struct stat st;
    bool valid;
};

struct bx_fuser_hit {
    pid_t pid;
    uid_t uid;
    char* command;
    bool write_access;
};

struct bx_fuser_hit_list {
    struct bx_fuser_hit* items;
    size_t len;
    size_t cap;
};

static void bx_fuser_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-fIMuvw] [-a|-s] [-4|-6] [-c|-m|-n SPACE]\n", progname);
    fprintf(stream, "       %s [-k [-i] [-SIGNAL]] NAME...\n", progname);
    fprintf(stream, "       %s -l\n", progname);
    fprintf(stream, "Show which processes use the named files, sockets, or filesystems.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all              display unused files too\n");
    fprintf(stream, "  -c, -m, --mount        show all processes using the named filesystem\n");
    fprintf(stream, "  -f                     ignored for compatibility\n");
    fprintf(stream, "  -i, --interactive      ask before killing (with -k)\n");
    fprintf(stream, "  -I, --inode            compare by inode even through path aliases\n");
    fprintf(stream, "  -k, --kill             kill processes accessing the named file\n");
    fprintf(stream, "  -l, --list-signals     list available signal names\n");
    fprintf(stream, "  -M, --ismountpoint     require NAME to be a mount point\n");
    fprintf(stream, "  -n, --namespace SPACE  search the file, tcp, or udp namespace\n");
    fprintf(stream, "  -s, --silent           silent operation\n");
    fprintf(stream, "  -u, --user             display user IDs\n");
    fprintf(stream, "  -v, --verbose          verbose output\n");
    fprintf(stream, "  -w, --writeonly        kill only processes with write access\n");
    fprintf(stream, "  -4, --ipv4             search IPv4 sockets only\n");
    fprintf(stream, "  -6, --ipv6             search IPv6 sockets only\n");
    fprintf(stream, "  -SIGNAL                send this signal instead of KILL\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

static void bx_fuser_hit_list_free(struct bx_fuser_hit_list* list) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        free(list->items[i].command);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

static struct bx_fuser_hit* bx_fuser_find_hit(struct bx_fuser_hit_list* list, pid_t pid) {
    size_t i;
    for (i = 0u; i < list->len; i++) {
        if (list->items[i].pid == pid) {
            return &list->items[i];
        }
    }
    return NULL;
}

static void bx_fuser_add_hit(struct bx_fuser_hit_list* list,
                             pid_t pid,
                             uid_t uid,
                             const char* command,
                             bool write_access) {
    struct bx_fuser_hit* hit = bx_fuser_find_hit(list, pid);
    if (hit != NULL) {
        hit->write_access = hit->write_access || write_access;
        return;
    }
    if (list->len == list->cap) {
        size_t next_cap = list->cap ? list->cap * 2u : 16u;
        list->items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->cap = next_cap;
    }
    list->items[list->len].pid = pid;
    list->items[list->len].uid = uid;
    list->items[list->len].command = xstrdup(command != NULL ? command : "?");
    list->items[list->len].write_access = write_access;
    list->len++;
}

static bool bx_fuser_is_signal_short_option(const char* arg) {
    static const char* known = "acfIiKlMnsuvw46hV";
    if (arg == NULL || arg[0] != '-' || arg[1] == '\0' || arg[1] == '-') {
        return false;
    }
    return strchr(known, arg[1]) == NULL || (arg[1] >= '0' && arg[1] <= '9');
}

static bool bx_fuser_parse_options(struct bx_fuser_options* options,
                                   int argc,
                                   char** argv,
                                   struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"mount", no_argument, NULL, 'm'},
        {"interactive", no_argument, NULL, 'i'},
        {"inode", no_argument, NULL, 'I'},
        {"kill", no_argument, NULL, 'k'},
        {"list-signals", no_argument, NULL, 'l'},
        {"ismountpoint", no_argument, NULL, 'M'},
        {"namespace", required_argument, NULL, 'n'},
        {"silent", no_argument, NULL, 's'},
        {"user", no_argument, NULL, 'u'},
        {"verbose", no_argument, NULL, 'v'},
        {"writeonly", no_argument, NULL, 'w'},
        {"ipv4", no_argument, NULL, '4'},
        {"ipv6", no_argument, NULL, '6'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;

    memset(options, 0, sizeof(*options));
    options->signal_number = SIGKILL;
    options->namespace_name = "file";

    if (argc > 1 && bx_fuser_is_signal_short_option(argv[1])) {
        options->signal_number = get_signal(argv[1] + 1, diag->progname);
        argv[1] = (char*)"--";
    }

    opterr = 0;
    optind = 1;
    while ((c = getopt_long(argc, argv, "+acfiIklMn:suvw46hV", long_options, NULL)) != -1) {
        switch (c) {
            case 'a': options->all = true; break;
            case 'c':
            case 'm': options->mount = true; break;
            case 'f': break;
            case 'i': options->interactive = true; break;
            case 'I': options->inode = true; break;
            case 'k': options->kill = true; break;
            case 'l': options->list_signals = true; break;
            case 'M': options->ismountpoint = true; break;
            case 'n': options->namespace_name = optarg; break;
            case 's': options->silent = true; break;
            case 'u': options->user = true; break;
            case 'v': options->verbose = true; break;
            case 'w': options->writeonly = true; break;
            case '4': options->ipv4 = true; break;
            case '6': options->ipv6 = true; break;
            case 'h':
            case 'V':
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

    options->first_name_index = optind;
    if (!options->list_signals && options->first_name_index >= argc) {
        bx_diag(diag, "missing file operand");
        return false;
    }
    if (strcmp(options->namespace_name, "file") != 0
        && strcmp(options->namespace_name, "tcp") != 0
        && strcmp(options->namespace_name, "udp") != 0) {
        bx_diag(diag, "unsupported namespace '%s'", options->namespace_name);
        return false;
    }
    if (strcmp(options->namespace_name, "file") != 0) {
        bx_diag(diag, "network namespace scanning is not supported yet");
        return false;
    }
    if (options->ipv4 || options->ipv6) {
        bx_diag(diag, "IPv4/IPv6 socket scanning is not supported yet");
        return false;
    }
    return true;
}

static bool bx_fuser_is_mountpoint(const char* path) {
    struct stat st;
    struct stat parent_st;
    char* parent;
    bool result;

    if (strcmp(path, "/") == 0) {
        return true;
    }
    if (stat(path, &st) != 0) {
        return false;
    }
    parent = bx_path_parent_dir_dup(path);
    if (parent == NULL) {
        return false;
    }
    result = false;
    if (stat(parent, &parent_st) == 0) {
        result = (st.st_dev != parent_st.st_dev) || (st.st_ino == parent_st.st_ino);
    }
    free(parent);
    return result;
}

static bool bx_fuser_stat_matches(const struct bx_fuser_options* options,
                                  const struct bx_fuser_target* target,
                                  const struct stat* st) {
    if (options->mount) {
        return target->st.st_dev == st->st_dev;
    }
    return target->st.st_dev == st->st_dev && target->st.st_ino == st->st_ino;
}

#define BX_FUSER_CMD_STORAGE 256u

static const char* bx_fuser_command_name(const struct bx_proc_info* info, char* storage, size_t storage_size) {
    size_t len;
    const char* base;
    if (info->cmdline != NULL && info->cmdline[0] != '\0') {
        const char* space = strchr(info->cmdline, ' ');
        len = space != NULL ? (size_t)(space - info->cmdline) : strlen(info->cmdline);
        if (len >= storage_size) {
            len = storage_size - 1u;
        }
        memcpy(storage, info->cmdline, len);
        storage[len] = '\0';
        base = strrchr(storage, '/');
        return base != NULL && base[1] != '\0' ? base + 1 : storage;
    }
    return info->comm != NULL ? info->comm : "?";
}

static bool bx_fuser_collect_target_hits(const struct bx_fuser_options* options,
                                         const struct bx_fuser_target* target,
                                         const struct bx_proc_list* procs,
                                         struct bx_fuser_hit_list* hits) {
    size_t i;
    for (i = 0u; i < procs->len; i++) {
        const struct bx_proc_info* info = &procs->items[i];
        struct bx_proc_fd_list fds = {0};
        bool vanished = false;
        bool matched = false;
        bool write_access = false;
        size_t j;
        char path[128];
        struct stat st;
        char command_storage[BX_FUSER_CMD_STORAGE];

        if (!bx_proc_read_fds(info->pid, &fds, &vanished)) {
            continue;
        }
        for (j = 0u; j < fds.len; j++) {
            if (!fds.items[j].have_stat) {
                continue;
            }
            if (!bx_fuser_stat_matches(options, target, &fds.items[j].st)) {
                continue;
            }
            matched = true;
            if (!fds.items[j].have_flags || (fds.items[j].flags & O_ACCMODE) != O_RDONLY) {
                write_access = true;
            }
            if (!options->mount) {
                break;
            }
        }
        if (!matched) {
            snprintf(path, sizeof(path), "/proc/%ld/cwd", (long)info->pid);
            if (stat(path, &st) == 0 && bx_fuser_stat_matches(options, target, &st)) {
                matched = true;
            }
        }
        if (!matched) {
            snprintf(path, sizeof(path), "/proc/%ld/root", (long)info->pid);
            if (stat(path, &st) == 0 && bx_fuser_stat_matches(options, target, &st)) {
                matched = true;
            }
        }
        if (!matched) {
            snprintf(path, sizeof(path), "/proc/%ld/exe", (long)info->pid);
            if (stat(path, &st) == 0 && bx_fuser_stat_matches(options, target, &st)) {
                matched = true;
            }
        }
        if (matched) {
            bx_fuser_add_hit(hits,
                             info->pid,
                             info->uid,
                             bx_fuser_command_name(info, command_storage, sizeof(command_storage)),
                             write_access);
        }
        bx_proc_fd_list_free(&fds);
    }
    return true;
}

static int bx_fuser_emit_hits(const struct bx_fuser_options* options,
                              const struct bx_fuser_target* target,
                              const struct bx_fuser_hit_list* hits) {
    size_t i;

    if (options->silent) {
        return 0;
    }
    if (options->verbose) {
        printf("%-8s %-12s %-8s %s\n", "PID", "USER", "ACCESS", "COMMAND");
        for (i = 0u; i < hits->len; i++) {
            char user_buf[32];
            const char* user_name = bx_id_user_name(hits->items[i].uid, user_buf);
            printf("%-8ld %-12s %-8s %s\n",
                   (long)hits->items[i].pid,
                   user_name,
                   hits->items[i].write_access ? "write" : "read",
                   hits->items[i].command);
        }
        return 0;
    }

    if (hits->len == 0u && !options->all) {
        return 0;
    }
    if (options->all || hits->len > 0u) {
        printf("%s:", target->name);
        for (i = 0u; i < hits->len; i++) {
            printf(" %ld", (long)hits->items[i].pid);
            if (options->user) {
                char user_buf[32];
                printf("(%s)", bx_id_user_name(hits->items[i].uid, user_buf));
            }
        }
        putchar('\n');
    }
    return 0;
}

static int bx_fuser_kill_hits(const struct bx_fuser_options* options,
                              const struct bx_fuser_hit_list* hits) {
    size_t i;
    int rc = 1;
    for (i = 0u; i < hits->len; i++) {
        pid_t signal_pid;
        bool vanished = false;
        char prompt[256];
        if (options->writeonly && !hits->items[i].write_access) {
            continue;
        }
        if (options->interactive) {
            snprintf(prompt, sizeof(prompt), "Kill process %ld (%s)? ", (long)hits->items[i].pid, hits->items[i].command);
            if (!bx_prompt_confirm(prompt)) {
                continue;
            }
        }
        if (!bx_proc_read_ns_pid(hits->items[i].pid, &signal_pid, &vanished)) {
            continue;
        }
        if (kill(signal_pid, options->signal_number) == 0) {
            rc = 0;
        }
    }
    return rc;
}

int bx_fuser_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "fuser"),
        .exit_status = 0,
    };
    struct bx_fuser_options options;
    struct bx_proc_list procs = {0};
    struct bx_fuser_target* targets = NULL;
    int handled;
    int i;
    int rc = 1;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "fuser", "-h", bx_fuser_print_help);
    if (handled >= 0) {
        return handled;
    }
    if (!bx_fuser_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    if (options.list_signals) {
        list_signals();
        return 0;
    }

    targets = xmalloc((size_t)(argc - options.first_name_index) * sizeof(*targets));
    for (i = options.first_name_index; i < argc; i++) {
        struct bx_fuser_target* target = &targets[i - options.first_name_index];
        target->name = argv[i];
        target->valid = false;
        if (stat(argv[i], &target->st) != 0) {
            if (!options.silent) {
                bx_diag(&diag, "%s: %s", argv[i], strerror(errno));
            }
            continue;
        }
        if (options.ismountpoint && !bx_fuser_is_mountpoint(argv[i])) {
            bx_diag(&diag, "%s is not a mountpoint", argv[i]);
            continue;
        }
        target->valid = true;
    }

    if (!bx_proc_list_read(&procs, BX_PROC_READ_CMDLINE)) {
        bx_diag(&diag, "failed to read /proc: %s", strerror(errno));
        free(targets);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    for (i = 0; i < argc - options.first_name_index; i++) {
        struct bx_fuser_hit_list hits = {0};
        if (!targets[i].valid) {
            continue;
        }
        bx_fuser_collect_target_hits(&options, &targets[i], &procs, &hits);
        if (hits.len > 0u) {
            rc = 0;
        }
        if (options.kill) {
            if (bx_fuser_kill_hits(&options, &hits) == 0) {
                rc = 0;
            }
        }
        else {
            bx_fuser_emit_hits(&options, &targets[i], &hits);
        }
        bx_fuser_hit_list_free(&hits);
    }

    bx_proc_list_free(&procs);
    free(targets);
    return rc == 0 ? 0 : (diag.exit_status != 0 ? diag.exit_status : 1);
}
