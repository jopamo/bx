#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/args_common.h"

struct bx_peekfd_options {
    bool eight_bit_clean;
    bool follow;
    bool duplicates_removed;
    bool no_headers;
    bool tgid;
    int pid_index;
};

static void bx_peekfd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-8] [-n] [-c] [-t] [-d] PID [FD ...]\n", progname);
    fprintf(stream, "Inspect currently open file descriptors of the selected process.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -8, --eight-bit-clean      accepted for compatibility\n");
    fprintf(stream, "  -c, --follow               accepted for compatibility\n");
    fprintf(stream, "  -d, --duplicates-removed   suppress duplicate descriptor targets\n");
    fprintf(stream, "  -n, --no-headers           print only descriptor targets\n");
    fprintf(stream, "  -t, --tgid                 accepted for compatibility\n");
    fprintf(stream, "  -h, --help                 display this help and exit\n");
    fprintf(stream, "  -V, --version              output version information and exit\n");
}

static bool bx_peekfd_parse_options(struct bx_peekfd_options* options,
                                    int argc,
                                    char** argv,
                                    struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"eight-bit-clean", no_argument, NULL, '8'},
        {"no-headers", no_argument, NULL, 'n'},
        {"follow", no_argument, NULL, 'c'},
        {"duplicates-removed", no_argument, NULL, 'd'},
        {"tgid", no_argument, NULL, 't'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;

    memset(options, 0, sizeof(*options));
    bx_args_getopt_reset();

    while ((c = bx_args_getopt_long(argc, argv, "+8ncdthV", long_options, NULL)) != -1) {
        switch (c) {
            case '8':
                options->eight_bit_clean = true;
                break;
            case 'n':
                options->no_headers = true;
                break;
            case 'c':
                options->follow = true;
                break;
            case 'd':
                options->duplicates_removed = true;
                break;
            case 't':
                options->tgid = true;
                break;
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

    options->pid_index = optind;
    if (options->pid_index >= argc) {
        bx_diag(diag, "missing process id");
        return false;
    }
    return true;
}

static bool bx_peekfd_parse_fd_operand(const char* text, int* fd_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-' || fd_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 0 || value > INT_MAX) {
        return false;
    }

    *fd_out = (int)value;
    return true;
}

static bool bx_peekfd_fd_selected(const struct bx_peekfd_options* options, int argc, char** argv, int fd) {
    int i;
    if (options->pid_index + 1 >= argc) {
        return true;
    }
    for (i = options->pid_index + 1; i < argc; i++) {
        int value = -1;
        if (bx_peekfd_parse_fd_operand(argv[i], &value) && value == fd) {
            return true;
        }
    }
    return false;
}

static bool bx_peekfd_seen_target(char** seen, size_t seen_len, const char* target) {
    size_t i;
    for (i = 0u; i < seen_len; i++) {
        if (strcmp(seen[i], target) == 0) {
            return true;
        }
    }
    return false;
}

int bx_peekfd_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "peekfd"),
        .exit_status = 0,
    };
    struct bx_peekfd_options options;
    struct bx_proc_fd_list fds = {0};
    char** seen = NULL;
    size_t seen_len = 0u;
    bool vanished = false;
    pid_t pid;
    int handled;
    size_t i;
    int rc = 1;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "peekfd", "-h", bx_peekfd_print_help);
    if (handled >= 0) {
        return handled;
    }
    if (!bx_peekfd_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    if (!bx_proc_parse_pid_arg(argv[options.pid_index], &pid)) {
        bx_diag(&diag, "invalid process id: %s", argv[options.pid_index]);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }
    for (i = options.pid_index + 1u; i < (size_t)argc; i++) {
        int fd = -1;
        if (!bx_peekfd_parse_fd_operand(argv[i], &fd)) {
            bx_diag(&diag, "invalid file descriptor: %s", argv[i]);
            return diag.exit_status != 0 ? diag.exit_status : 1;
        }
    }
    if (!bx_proc_read_fds(pid, &fds, &vanished)) {
        if (vanished || errno == ENOENT) {
            bx_diag(&diag, "%ld: process not found", (long)pid);
        }
        else {
            bx_diag(&diag, "%ld: %s", (long)pid, strerror(errno));
        }
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    for (i = 0u; i < fds.len; i++) {
        struct bx_proc_fd_entry* entry = &fds.items[i];
        if (!bx_peekfd_fd_selected(&options, argc, argv, entry->fd)) {
            continue;
        }
        if (options.duplicates_removed && bx_peekfd_seen_target(seen, seen_len, entry->target)) {
            continue;
        }
        if (options.duplicates_removed) {
            seen = realloc(seen, (seen_len + 1u) * sizeof(*seen));
            seen[seen_len++] = entry->target;
        }
        if (options.no_headers) {
            puts(entry->target);
        }
        else {
            printf("fd %d: %s", entry->fd, entry->target);
            if (entry->have_position) {
                printf(" pos=%lld", (long long)entry->position);
            }
            if (entry->have_flags) {
                printf(" flags=0%lo", entry->flags);
            }
            putchar('\n');
        }
        rc = 0;
    }

    free(seen);
    bx_proc_fd_list_free(&fds);
    return rc;
}
