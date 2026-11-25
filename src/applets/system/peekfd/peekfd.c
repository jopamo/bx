#include <errno.h>
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
#include "lib/option_scan.h"

struct bx_peekfd_options {
    bool duplicates_removed;
    bool no_headers;
    bool show_help;
    bool show_version;
    const char* pid_arg;
    const char* bad_fd_arg;
    int* selected_fds;
    size_t selected_count;
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

static bool bx_peekfd_parse_options(struct bx_peekfd_options* options,
                                    int argc,
                                    char** argv,
                                    struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'8', '8', "eight-bit-clean", BX_OPTION_NO_VALUE},
        {'n', 'n', "no-headers", BX_OPTION_NO_VALUE},
        {'c', 'c', "follow", BX_OPTION_NO_VALUE},
        {'d', 'd', "duplicates-removed", BX_OPTION_NO_VALUE},
        {'t', 't', "tgid", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!options->pid_arg) {
                options->pid_arg = option.token;
                continue;
            }
            if (options->bad_fd_arg)
                continue;
            int fd;
            if (!bx_peekfd_parse_fd_operand(option.token, &fd)) {
                options->bad_fd_arg = option.token;
                continue;
            }
            if (!options->selected_fds) {
                options->selected_fds = calloc((size_t)argc - option.index, sizeof(*options->selected_fds));
                if (!options->selected_fds) {
                    bx_diag(diag, "allocate descriptor selectors: %s", strerror(errno));
                    return false;
                }
            }
            options->selected_fds[options->selected_count++] = fd;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        switch (option.spec->id) {
            case '8':
            case 'c':
            case 't':
                break;
            case 'n':
                options->no_headers = true;
                break;
            case 'd':
                options->duplicates_removed = true;
                break;
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    if (!options->pid_arg) {
        bx_diag(diag, "missing process id");
        return false;
    }
    return true;
}

static bool bx_peekfd_fd_selected(const struct bx_peekfd_options* options, int fd) {
    if (options->selected_count == 0)
        return true;
    for (size_t i = 0; i < options->selected_count; i++) {
        if (options->selected_fds[i] == fd)
            return true;
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
    struct bx_peekfd_options options = {0};
    struct bx_proc_fd_list fds = {0};
    char** seen = NULL;
    size_t seen_len = 0u;
    bool vanished = false;
    pid_t pid;
    size_t i;
    int rc = 1;

    if (!bx_peekfd_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0)
            bx_cli_print_try_help(diag.progname);
        goto cleanup;
    }
    if (options.show_help) {
        bx_peekfd_print_help(stdout, diag.progname);
        rc = 0;
        goto cleanup;
    }
    if (options.show_version) {
        bx_cli_print_version(diag.progname);
        rc = 0;
        goto cleanup;
    }
    if (!bx_proc_parse_pid_arg(options.pid_arg, &pid)) {
        bx_diag(&diag, "invalid process id: %s", options.pid_arg);
        goto cleanup;
    }
    if (options.bad_fd_arg) {
        bx_diag(&diag, "invalid file descriptor: %s", options.bad_fd_arg);
        goto cleanup;
    }
    if (!bx_proc_read_fds(pid, &fds, &vanished)) {
        if (vanished || errno == ENOENT)
            bx_diag(&diag, "%ld: process not found", (long)pid);
        else
            bx_diag(&diag, "%ld: %s", (long)pid, strerror(errno));
        goto cleanup;
    }
    if (options.duplicates_removed && fds.len) {
        seen = calloc(fds.len, sizeof(*seen));
        if (!seen) {
            bx_diag(&diag, "allocate descriptor targets: %s", strerror(errno));
            goto cleanup;
        }
    }

    for (i = 0u; i < fds.len; i++) {
        struct bx_proc_fd_entry* entry = &fds.items[i];
        if (!bx_peekfd_fd_selected(&options, entry->fd)) {
            continue;
        }
        if (options.duplicates_removed && bx_peekfd_seen_target(seen, seen_len, entry->target)) {
            continue;
        }
        if (options.duplicates_removed)
            seen[seen_len++] = entry->target;
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

cleanup:
    free(options.selected_fds);
    free(seen);
    bx_proc_fd_list_free(&fds);
    return diag.exit_status != 0 ? diag.exit_status : rc;
}
