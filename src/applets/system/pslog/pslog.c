#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void bx_pslog_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [--] PID...\n", progname);
    fprintf(stream, "Print log file paths currently opened by each PID.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

static bool bx_pslog_target_is_log(const char* target) {
    size_t len;
    if (target == NULL) {
        return false;
    }
    len = strlen(target);
    return len >= 3u && strcmp(target + len - 3u, "log") == 0;
}

int bx_pslog_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "pslog"),
        .exit_status = 0,
    };
    int handled;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "pslog", "-h", bx_pslog_print_help);
    if (handled >= 0) {
        return handled;
    }
    int first = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    if (argc <= first) {
        bx_diag(&diag, "missing process id");
        bx_cli_print_try_help(diag.progname);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    const size_t count = (size_t)(argc - first);
    pid_t* pids = calloc(count, sizeof(*pids));
    if (!pids) {
        bx_diag(&diag, "memory exhausted");
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        const char* arg = argv[(size_t)first + i];
        if (!bx_proc_parse_pid_arg(arg, &pids[i])) {
            bx_diag(&diag, "invalid process id: %s", arg);
            free(pids);
            return 1;
        }
    }

    int rc = 0;
    for (size_t i = 0; i < count; i++) {
        struct bx_proc_fd_list fds = {0};
        bool vanished = false;
        bool printed_any = false;
        pid_t pid = pids[i];
        if (!bx_proc_read_fds(pid, &fds, &vanished)) {
            if (vanished || errno == ENOENT) {
                bx_diag(&diag, "%ld: process not found", (long)pid);
            }
            else {
                bx_diag(&diag, "%ld: %s", (long)pid, strerror(errno));
            }
            rc = 1;
            break;
        }

        printf("Pid no %ld:\n", (long)pid);
        for (size_t j = 0u; j < fds.len; j++) {
            if (!bx_pslog_target_is_log(fds.items[j].target)) {
                continue;
            }
            printf("Log path: %s\n", fds.items[j].target);
            printed_any = true;
        }
        if (!printed_any) {
            printf("Log path: (none)\n");
        }
        bx_proc_fd_list_free(&fds);
    }

    free(pids);
    return rc;
}
