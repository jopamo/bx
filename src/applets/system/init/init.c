/*
 * BusyBox init-compatible policy frontend.
 * Adapted from BusyBox init/init.c at bee252057c7ac69909b8aafeafb8e414e34c7685.
 * Copyright (C) 1995, 1996 Bruce Perens; Copyright (C) 1999-2004 Erik Andersen.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdio.h>
#include <string.h>

#include "applets.h"
#include "applets/system/init/init_internal.h"
#include "lib/cli_common.h"

static void bx_init_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "BusyBox v1.38.0.git () multi-call binary.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Usage: %s\n", progname);
    fprintf(stream, "\n");
    fprintf(stream, "Init is the first process started during boot. It never exits.\n");
    fprintf(stream, "It (re)spawns children according to /etc/inittab.\n");
    fprintf(stream, "Signals:\n");
    fprintf(stream, "HUP: reload /etc/inittab\n");
    fprintf(stream, "TSTP: stop respawning until CONT\n");
    fprintf(stream, "QUIT: re-exec another init\n");
    fprintf(stream, "USR1/TERM/USR2/INT: run halt/reboot/poweroff/Ctrl-Alt-Del script\n");
}

int bx_init_main(int argc, char **argv) {
    const char *progname = bx_cli_progname(
        argc > 0 ? argv[0] : NULL, "init");
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        if (strcmp(progname, "linuxrc") == 0) {
            fprintf(stdout,
                    "BusyBox v1.38.0.git () multi-call binary.\n\n"
                    "No help available\n");
        } else {
            bx_init_print_help(stdout, progname);
        }
        return 0;
    }
    return bx_init_run(argc, argv, progname);
}
