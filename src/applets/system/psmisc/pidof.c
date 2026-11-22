#define _XOPEN_SOURCE 700

#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-q] [-s] [--] PROGRAM...\n"
                 "Find processes by name or executable path.\n"
                 "  -q, --quiet         only set the exit status\n"
                 "  -s, --single-shot   report only one matching PID\n", name);
}

#ifdef __linux__
#include <getopt.h>
#include <stdlib.h>
#include <string.h>

#include "applets/system/psmisc/procfs.h"
#include "lib/args_common.h"
#include "lib/path_ops.h"

struct pidof_query {
    const char* name;
    char* path;
};

static bool pidof_matches(const struct bx_proc_info* process,
                          const struct pidof_query* queries, size_t count) {
    if (process->state == 'Z' || process->state == 'X')
        return false;
    const char* base = process->exe ? bx_path_basename_ptr(process->exe) : NULL;
    for (size_t i = 0; i < count; i++) {
        if (queries[i].name) {
            if ((process->comm && strcmp(queries[i].name, process->comm) == 0) ||
                (base && strcmp(queries[i].name, base) == 0))
                return true;
        } else if (queries[i].path && process->exe && strcmp(queries[i].path, process->exe) == 0) {
            return true;
        }
    }
    return false;
}
#endif

int bx_pidof_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "pidof", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "pidof"};
#ifdef __linux__
    static const struct option long_options[] = {
        {"quiet", no_argument, NULL, 'q'},
        {"single-shot", no_argument, NULL, 's'},
        {NULL, 0, NULL, 0},
    };
    bool quiet = false, single = false;
    bx_args_getopt_reset();
    for (int opt; (opt = bx_args_getopt_long(argc, argv, "qs", long_options, NULL)) != -1;) {
        switch (opt) {
        case 'q': quiet = true; break;
        case 's': single = true; break;
        default:
            bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (optind == argc)
        return 1;

    size_t count = (size_t)(argc - optind);
    struct pidof_query* queries = calloc(count, sizeof(*queries));
    if (queries == NULL) {
        bx_perror_path(&diag, "process queries");
        return 1;
    }
    for (size_t i = 0; i < count; i++) {
        const char* arg = argv[(size_t)optind + i];
        if (strchr(arg, '/') == NULL) {
            queries[i].name = arg;
        } else {
            queries[i].path = realpath(arg, NULL);
        }
    }

    int status = 1;
    struct bx_proc_list processes;
    if (!bx_proc_list_read(&processes, BX_PROC_READ_EXE)) {
        bx_perror_path(&diag, "/proc");
        goto done;
    }
    /* The snapshot is PID-sorted; visit it once so overlaps print only once. */
    bool found = false;
    for (size_t i = processes.len; i > 0; i--) {
        const struct bx_proc_info* process = &processes.items[i - 1];
        if (!pidof_matches(process, queries, count))
            continue;
        if (!quiet && printf("%s%ld", found ? " " : "", (long)process->pid) < 0)
            goto output_error;
        found = true;
        if (quiet || single)
            break;
    }
    if (found) {
        if (!quiet && (putchar('\n') == EOF || fflush(stdout) == EOF))
            goto output_error;
        status = 0;
    }
    goto free_processes;

output_error:
    bx_perror_path(&diag, "standard output");
free_processes:
    bx_proc_list_free(&processes);
done:
    for (size_t i = 0; i < count; i++)
        free(queries[i].path);
    free(queries);
    return status;
#else
    bx_diag(&diag, "Linux process lookup is not supported on this platform");
    return 1;
#endif
}
