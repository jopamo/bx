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
#include "lib/option_scan.h"

struct bx_prtstat_options {
    bool raw;
    bool show_help;
    bool show_version;
    pid_t* pids;
    size_t pid_count;
    const char* invalid_pid;
};

static void bx_prtstat_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... PID...\n", progname);
    fprintf(stream, "Print decoded /proc/PID/stat information.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -r, --raw              dump raw stat fields\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

static bool bx_prtstat_parse_options(struct bx_prtstat_options* options,
                                     int argc,
                                     char** argv,
                                     struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'r', 'r', "raw", BX_OPTION_NO_VALUE},
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (options->invalid_pid)
                continue;
            pid_t pid;
            if (!bx_proc_parse_pid_arg(option.token, &pid)) {
                options->invalid_pid = option.token;
                continue;
            }
            if (!options->pids) {
                options->pids = calloc((size_t)argc - option.index, sizeof(*options->pids));
                if (!options->pids) {
                    bx_diag(diag, "allocate process ids: %s", strerror(errno));
                    return false;
                }
            }
            options->pids[options->pid_count++] = pid;
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
            case 'r':
                options->raw = true;
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

    if (options->pid_count == 0 && !options->invalid_pid) {
        bx_diag(diag, "missing process id");
        return false;
    }
    return true;
}

static const char* bx_prtstat_state_description(char state) {
    switch (state) {
        case 'R': return "running";
        case 'S': return "sleeping";
        case 'D': return "disk sleep";
        case 'Z': return "zombie";
        case 'T': return "stopped";
        case 't': return "tracing stop";
        case 'X':
        case 'x': return "dead";
        case 'I': return "idle";
        case 'P': return "parked";
        default: return "unknown";
    }
}

static int bx_prtstat_print_one(pid_t pid, const struct bx_prtstat_options* options, struct bx_diag_ctx* diag) {
    bool vanished = false;

    if (options->raw) {
        char* text = NULL;
        if (!bx_proc_read_text_file(pid, "stat", &text, &vanished)) {
            if (vanished || errno == ENOENT) {
                bx_diag(diag, "%ld: process not found", (long)pid);
            }
            else {
                bx_diag(diag, "%ld: %s", (long)pid, strerror(errno));
            }
            return 1;
        }
        fputs(text, stdout);
        if (text[0] != '\0' && text[strlen(text) - 1u] != '\n') {
            fputc('\n', stdout);
        }
        free(text);
        return 0;
    }
    else {
        struct bx_proc_stat stat_info;
        long ticks_per_second = bx_proc_clock_ticks_per_second();
        if (!bx_proc_read_stat(pid, &stat_info, &vanished)) {
            if (vanished || errno == ENOENT) {
                bx_diag(diag, "%ld: process not found", (long)pid);
            }
            else {
                bx_diag(diag, "%ld: %s", (long)pid, strerror(errno));
            }
            return 1;
        }

        printf("Process: %s\n", stat_info.comm);
        printf("State: %c (%s)\n", stat_info.state, bx_prtstat_state_description(stat_info.state));
        printf("PID: %ld\n", (long)stat_info.pid);
        printf("PPID: %ld\n", (long)stat_info.ppid);
        printf("Process group: %ld\n", (long)stat_info.pgrp);
        printf("Session: %ld\n", (long)stat_info.session);
        printf("TTY nr: %ld\n", stat_info.tty_nr);
        printf("TPGID: %ld\n", stat_info.tpgid);
        printf("Flags: 0x%lx\n", stat_info.flags);
        printf("User time (ticks): %llu\n", stat_info.utime_ticks);
        printf("System time (ticks): %llu\n", stat_info.stime_ticks);
        printf("User time (seconds): %.3f\n", (double)stat_info.utime_ticks / (double)ticks_per_second);
        printf("System time (seconds): %.3f\n", (double)stat_info.stime_ticks / (double)ticks_per_second);
        printf("Priority: %ld\n", stat_info.priority);
        printf("Nice: %ld\n", stat_info.nice);
        printf("Threads: %ld\n", stat_info.num_threads);
        printf("Start time (ticks since boot): %llu\n", stat_info.starttime_ticks);
        printf("Virtual size: %llu bytes\n", stat_info.vsize_bytes);
        printf("Resident set: %lld pages\n", stat_info.rss_pages);
        bx_proc_stat_free(&stat_info);
        return 0;
    }
}

int bx_prtstat_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {
        .progname = bx_psmisc_progname((argc > 0) ? argv[0] : NULL, "prtstat"),
        .exit_status = 0,
    };
    struct bx_prtstat_options options = {0};
    int rc = 1;

    if (!bx_prtstat_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0)
            bx_cli_print_try_help(diag.progname);
        goto cleanup;
    }
    if (options.show_help) {
        bx_prtstat_print_help(stdout, diag.progname);
        rc = 0;
        goto cleanup;
    }
    if (options.show_version) {
        bx_cli_print_version(diag.progname);
        rc = 0;
        goto cleanup;
    }
    if (options.invalid_pid) {
        bx_diag(&diag, "invalid process id: %s", options.invalid_pid);
        goto cleanup;
    }

    rc = 0;
    for (size_t i = 0; i < options.pid_count; i++) {
        if (i > 0)
            putchar('\n');
        if (bx_prtstat_print_one(options.pids[i], &options, &diag) != 0)
            rc = 1;
    }

cleanup:
    free(options.pids);
    return diag.exit_status != 0 ? diag.exit_status : rc;
}
