#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "applets/system/psmisc/procfs.h"
#include "applets/system/psmisc/psmisc_wrapper.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

struct bx_prtstat_options {
    bool raw;
    int first_pid_index;
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
    static const struct option long_options[] = {
        {"raw", no_argument, NULL, 'r'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };
    int c;

    memset(options, 0, sizeof(*options));
    optind = 1;
    opterr = 0;

    while ((c = getopt_long(argc, argv, "+rhV", long_options, NULL)) != -1) {
        switch (c) {
            case 'r':
                options->raw = true;
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

    options->first_pid_index = optind;
    if (options->first_pid_index >= argc) {
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
    struct bx_prtstat_options options;
    int handled;
    int i;
    int rc = 0;

    handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "prtstat", "-h", bx_prtstat_print_help);
    if (handled >= 0) {
        return handled;
    }
    if (!bx_prtstat_parse_options(&options, argc, argv, &diag)) {
        if (diag.exit_status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    for (i = options.first_pid_index; i < argc; i++) {
        pid_t pid;
        if (!bx_proc_parse_pid_arg(argv[i], &pid)) {
            bx_diag(&diag, "invalid process id: %s", argv[i]);
            return diag.exit_status != 0 ? diag.exit_status : 1;
        }
        if (i > options.first_pid_index) {
            putchar('\n');
        }
        if (bx_prtstat_print_one(pid, &options, &diag) != 0) {
            rc = 1;
        }
    }

    return rc;
}
