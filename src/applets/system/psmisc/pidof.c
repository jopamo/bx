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
#include <stdlib.h>
#include <string.h>

#include "applets/system/psmisc/procfs.h"
#include "lib/option_scan.h"
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
    struct bx_diag_ctx diag = {.progname = bx_cli_progname(argc > 0 ? argv[0] : NULL, "pidof")};
#ifdef __linux__
    static const struct bx_option_spec specs[] = {
        {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
        {'s', 's', "single-shot", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };
    bool quiet = false, single = false;
    struct pidof_query* queries = NULL;
    size_t count = 0;
    int status = 1;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (!queries) {
                queries = calloc((size_t)argc - option.index, sizeof(*queries));
                if (!queries) {
                    bx_perror_path(&diag, "process queries");
                    goto done;
                }
            }
            queries[count++].name = option.token;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(&diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", option.token);
            goto done;
        }
        switch (option.spec->id) {
            case 'q':
                quiet = true;
                break;
            case 's':
                single = true;
                break;
            case 'h':
                usage(stdout, diag.progname);
                status = 0;
                goto done;
            case 'V':
                bx_cli_print_version(diag.progname);
                status = 0;
                goto done;
        }
    }
    if (count == 0)
        goto done;

    for (size_t i = 0; i < count; i++) {
        const char* arg = queries[i].name;
        if (strchr(arg, '/')) {
            queries[i].path = realpath(arg, NULL);
            queries[i].name = NULL;
        }
    }

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
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "pidof", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    bx_diag(&diag, "Linux process lookup is not supported on this platform");
    return 1;
#endif
}
