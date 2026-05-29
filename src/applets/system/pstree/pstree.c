#include <errno.h>
#include <getopt.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

enum bx_pstree_charset {
    BX_PSTREE_CHARSET_ASCII = 0,
    BX_PSTREE_CHARSET_UNICODE,
};

struct bx_pstree_options {
    bool show_args;
    bool show_pids;
    bool numeric_sort;
    bool show_parents;
    bool show_paths;
    enum bx_pstree_charset charset;
    bool pid_filter_set;
    pid_t pid_filter;
    bool user_filter_set;
    uid_t user_filter;
};

struct bx_pstree_connectors {
    const char* branch;
    const char* last_branch;
    const char* vertical;
    const char* space;
};

static void bx_pstree_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-acglpsStTuZ] [-h | -H PID] [-n | -N TYPE]\n", progname);
    fprintf(stream, "       %s [-A | -G | -U] [PID | USER]\n", progname);
    fprintf(stream, "Display processes as a tree.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --arguments         show command line arguments\n");
    fprintf(stream, "  -A, --ascii             use ASCII line drawing characters\n");
    fprintf(stream, "  -c, --compact-not       accepted for compatibility\n");
    fprintf(stream, "  -C, --color=TYPE        accepted for compatibility\n");
    fprintf(stream, "  -g, --show-pgids        accepted for compatibility\n");
    fprintf(stream, "  -G, --vt100             use simple line drawing characters\n");
    fprintf(stream, "  -h, --highlight-all     accepted for compatibility\n");
    fprintf(stream, "  -H, --highlight-pid PID select PID\n");
    fprintf(stream, "  -k, --kthreads          accepted for compatibility\n");
    fprintf(stream, "  -l, --long              accepted for compatibility\n");
    fprintf(stream, "  -n, --numeric-sort      sort output by PID\n");
    fprintf(stream, "  -N, --ns-sort TYPE      accepted for compatibility\n");
    fprintf(stream, "  -p, --show-pids         show PIDs\n");
    fprintf(stream, "  -P, --show-paths        show executable paths when available\n");
    fprintf(stream, "  -s, --show-parents      show parents of the selected process\n");
    fprintf(stream, "  -S, --ns-changes        accepted for compatibility\n");
    fprintf(stream, "  -t, --thread-names      accepted for compatibility\n");
    fprintf(stream, "  -T, --hide-threads      accepted for compatibility\n");
    fprintf(stream, "  -u, --uid-changes       accepted for compatibility\n");
    fprintf(stream, "  -U, --unicode           use UTF-8 line drawing characters\n");
    fprintf(stream, "  -Z, --security-context  accepted for compatibility\n");
    fprintf(stream, "      --help              display this help and exit\n");
    fprintf(stream, "  -V, --version           output version information and exit\n");
}

static bool bx_pstree_parse_options(struct bx_pstree_options* options,
                                    int argc,
                                    char** argv,
                                    struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"arguments", no_argument, NULL, 'a'},
        {"ascii", no_argument, NULL, 'A'},
        {"compact-not", no_argument, NULL, 'c'},
        {"color", required_argument, NULL, 'C'},
        {"show-pgids", no_argument, NULL, 'g'},
        {"vt100", no_argument, NULL, 'G'},
        {"highlight-all", no_argument, NULL, 'h'},
        {"highlight-pid", required_argument, NULL, 'H'},
        {"kthreads", no_argument, NULL, 'k'},
        {"long", no_argument, NULL, 'l'},
        {"numeric-sort", no_argument, NULL, 'n'},
        {"ns-sort", required_argument, NULL, 'N'},
        {"show-pids", no_argument, NULL, 'p'},
        {"show-paths", no_argument, NULL, 'P'},
        {"show-parents", no_argument, NULL, 's'},
        {"ns-changes", no_argument, NULL, 'S'},
        {"thread-names", no_argument, NULL, 't'},
        {"hide-threads", no_argument, NULL, 'T'},
        {"uid-changes", no_argument, NULL, 'u'},
        {"unicode", no_argument, NULL, 'U'},
        {"security-context", no_argument, NULL, 'Z'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;

    memset(options, 0, sizeof(*options));
    options->charset = BX_PSTREE_CHARSET_ASCII;
    bx_args_getopt_reset();

    while ((c = bx_args_getopt_long(argc, argv, "+aAcC:gGhH:klnN:pPsStTuUZV", long_options, NULL)) != -1) {
        switch (c) {
            case 'a': options->show_args = true; break;
            case 'A':
            case 'G': options->charset = BX_PSTREE_CHARSET_ASCII; break;
            case 'U': options->charset = BX_PSTREE_CHARSET_UNICODE; break;
            case 'n': options->numeric_sort = true; break;
            case 'p': options->show_pids = true; break;
            case 'P': options->show_paths = true; break;
            case 's': options->show_parents = true; break;
            case 'H':
                if (!bx_proc_parse_pid_arg(optarg, &options->pid_filter)) {
                    bx_diag(diag, "invalid process id: %s", optarg);
                    return false;
                }
                options->pid_filter_set = true;
                break;
            case 'h':
            case 'c':
            case 'g':
            case 'k':
            case 'l':
            case 'S':
            case 't':
            case 'T':
            case 'u':
            case 'Z':
                break;
            case 'C':
            case 'N':
                break;
            case 1:
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

    if (optind < argc) {
        if (!options->pid_filter_set && bx_proc_parse_pid_arg(argv[optind], &options->pid_filter)) {
            options->pid_filter_set = true;
            optind++;
        }
        else {
            struct passwd* pw = getpwnam(argv[optind]);
            if (pw == NULL) {
                bx_diag(diag, "unknown user: %s", argv[optind]);
                return false;
            }
            options->user_filter = pw->pw_uid;
            options->user_filter_set = true;
            optind++;
        }
    }
    if (optind < argc) {
        bx_diag(diag, "unexpected operand '%s'", argv[optind]);
        return false;
    }
    if (options->show_parents && !options->pid_filter_set) {
        options->pid_filter = getpid();
        options->pid_filter_set = true;
    }
    return true;
}

static const struct bx_pstree_connectors* bx_pstree_connectors(enum bx_pstree_charset charset) {
    static const struct bx_pstree_connectors ascii = {
        .branch = "|-",
        .last_branch = "`-",
        .vertical = "| ",
        .space = "  ",
    };
    static const struct bx_pstree_connectors unicode = {
        .branch = "├─",
        .last_branch = "└─",
        .vertical = "│ ",
        .space = "  ",
    };
    return charset == BX_PSTREE_CHARSET_UNICODE ? &unicode : &ascii;
}

static ssize_t bx_pstree_find_index_by_pid(const struct bx_proc_list* procs, pid_t pid) {
    size_t i;
    for (i = 0u; i < procs->len; i++) {
        if (procs->items[i].pid == pid) {
            return (ssize_t)i;
        }
    }
    return -1;
}

static const char* bx_pstree_label(const struct bx_pstree_options* options,
                                   const struct bx_proc_info* info,
                                   char storage[1024]) {
    const char* base = info->comm != NULL ? info->comm : "?";
    if (options->show_paths && info->exe != NULL && info->exe[0] != '\0') {
        base = info->exe;
    }
    else if (options->show_args && info->cmdline != NULL && info->cmdline[0] != '\0') {
        base = info->cmdline;
    }
    if (options->show_pids) {
        snprintf(storage, 1024, "%s(%ld)", base, (long)info->pid);
        return storage;
    }
    return base;
}

static size_t bx_pstree_collect_children(const struct bx_proc_list* procs,
                                         const struct bx_pstree_options* options,
                                         size_t parent_index,
                                         size_t** children_out) {
    size_t* children = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    size_t i;
    (void)options;

    for (i = 0u; i < procs->len; i++) {
        if (procs->items[i].ppid != procs->items[parent_index].pid) {
            continue;
        }
        if (cap == len) {
            size_t next_cap = cap ? cap * 2u : 8u;
            children = xrealloc(children, next_cap * sizeof(*children));
            cap = next_cap;
        }
        children[len++] = i;
    }

    if (len > 1u) {
        size_t a, b;
        for (a = 0u; a < len; a++) {
            for (b = a + 1u; b < len; b++) {
                bool swap = false;
                if (options->numeric_sort) {
                    swap = procs->items[children[a]].pid > procs->items[children[b]].pid;
                }
                else {
                    int cmp = strcmp(procs->items[children[a]].comm != NULL ? procs->items[children[a]].comm : "",
                                     procs->items[children[b]].comm != NULL ? procs->items[children[b]].comm : "");
                    if (cmp > 0 || (cmp == 0 && procs->items[children[a]].pid > procs->items[children[b]].pid)) {
                        swap = true;
                    }
                }
                if (swap) {
                    size_t tmp = children[a];
                    children[a] = children[b];
                    children[b] = tmp;
                }
            }
        }
    }
    *children_out = children;
    return len;
}

static void bx_pstree_print_subtree(const struct bx_proc_list* procs,
                                    const struct bx_pstree_options* options,
                                    size_t index,
                                    const char* prefix,
                                    bool last,
                                    bool print_connector) {
    const struct bx_pstree_connectors* con = bx_pstree_connectors(options->charset);
    char next_prefix[4096];
    char label_storage[1024];
    size_t* children = NULL;
    size_t child_count;
    size_t i;

    if (print_connector) {
        printf("%s%s", prefix, last ? con->last_branch : con->branch);
    }
    else {
        fputs(prefix, stdout);
    }
    printf("%s\n", bx_pstree_label(options, &procs->items[index], label_storage));

    snprintf(next_prefix,
             sizeof(next_prefix),
             "%s%s",
             prefix,
             print_connector ? (last ? con->space : con->vertical) : "");
    child_count = bx_pstree_collect_children(procs, options, index, &children);
    for (i = 0u; i < child_count; i++) {
        bx_pstree_print_subtree(procs,
                                options,
                                children[i],
                                next_prefix,
                                i + 1u == child_count,
                                true);
    }
    free(children);
}

static int bx_pstree_print_ancestry(const struct bx_proc_list* procs,
                                    const struct bx_pstree_options* options,
                                    size_t leaf_index,
                                    struct bx_diag_ctx* diag) {
    size_t* path = NULL;
    size_t len = 0u;
    size_t cap = 0u;
    ssize_t current = (ssize_t)leaf_index;
    size_t i;

    while (current >= 0) {
        if (len == cap) {
            size_t next_cap = cap ? cap * 2u : 8u;
            path = xrealloc(path, next_cap * sizeof(*path));
            cap = next_cap;
        }
        path[len++] = (size_t)current;
        current = bx_pstree_find_index_by_pid(procs, procs->items[(size_t)current].ppid);
    }
    if (len == 0u) {
        bx_diag(diag, "selected process not found");
        free(path);
        return 1;
    }
    for (i = len; i > 0u; i--) {
        char label_storage[1024];
        size_t index = path[i - 1u];
        const char* label = bx_pstree_label(options, &procs->items[index], label_storage);
        if (i != len) {
            printf(" -> ");
        }
        printf("%s", label);
    }
    putchar('\n');
    free(path);
    return 0;
}

int bx_pstree_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "pstree"),
        .exit_status = 0,
    };
    struct bx_pstree_options options;
    struct bx_proc_list procs = {0};
    size_t i;
    int handled;
    int rc = 1;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "pstree", NULL, bx_pstree_print_help);
    if (handled >= 0) {
        return handled;
    }
    if (!bx_pstree_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    if (!bx_proc_list_read(&procs, BX_PROC_READ_CMDLINE | BX_PROC_READ_EXE)) {
        bx_diag(&diag, "failed to read /proc: %s", strerror(errno));
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.pid_filter_set) {
        ssize_t index = bx_pstree_find_index_by_pid(&procs, options.pid_filter);
        if (index < 0) {
            bx_diag(&diag, "%ld: process not found", (long)options.pid_filter);
            bx_proc_list_free(&procs);
            return diag.exit_status != 0 ? diag.exit_status : 1;
        }
        rc = options.show_parents
                 ? bx_pstree_print_ancestry(&procs, &options, (size_t)index, &diag)
                 : (bx_pstree_print_subtree(&procs, &options, (size_t)index, "", true, false), 0);
        bx_proc_list_free(&procs);
        return rc;
    }

    for (i = 0u; i < procs.len; i++) {
        bool parent_present = bx_pstree_find_index_by_pid(&procs, procs.items[i].ppid) >= 0;
        if (options.user_filter_set && procs.items[i].uid != options.user_filter) {
            continue;
        }
        if (parent_present && (!options.user_filter_set || procs.items[bx_pstree_find_index_by_pid(&procs, procs.items[i].ppid)].uid == options.user_filter)) {
            continue;
        }
        if (rc == 0) {
            putchar('\n');
        }
        bx_pstree_print_subtree(&procs, &options, i, "", true, false);
        rc = 0;
    }

    bx_proc_list_free(&procs);
    return rc == 0 ? 0 : (diag.exit_status != 0 ? diag.exit_status : 1);
}
