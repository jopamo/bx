#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "lib/argv_packer.h"
#include "lib/child_runner.h"
#include "xargs_exec.h"
#include "xargs_input.h"
#include "xargs_parse.h"

static volatile sig_atomic_t xargs_interrupt_signal = 0;

struct xargs_signal_handlers {
    struct sigaction old_int;
    struct sigaction old_term;
    struct sigaction old_hup;
    bool has_int;
    bool has_term;
    bool has_hup;
};

static void xargs_handle_interrupt_signal(int signo) {
    xargs_interrupt_signal = signo;
}

static int xargs_install_one_signal_handler(int signo, struct sigaction *old_action) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = xargs_handle_interrupt_signal;
    sigemptyset(&sa.sa_mask);
    return sigaction(signo, &sa, old_action);
}

static int xargs_install_signal_handlers(const char *progname,
                                         struct xargs_signal_handlers *handlers) {
    memset(handlers, 0, sizeof(*handlers));
    xargs_interrupt_signal = 0;

    if (xargs_install_one_signal_handler(SIGINT, &handlers->old_int) != 0) {
        fprintf(stderr, "%s: cannot install SIGINT handler: %s\n", progname, strerror(errno));
        return 1;
    }
    handlers->has_int = true;

    if (xargs_install_one_signal_handler(SIGTERM, &handlers->old_term) != 0) {
        fprintf(stderr, "%s: cannot install SIGTERM handler: %s\n", progname, strerror(errno));
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_int = false;
        return 1;
    }
    handlers->has_term = true;

    if (xargs_install_one_signal_handler(SIGHUP, &handlers->old_hup) != 0) {
        fprintf(stderr, "%s: cannot install SIGHUP handler: %s\n", progname, strerror(errno));
        sigaction(SIGTERM, &handlers->old_term, NULL);
        sigaction(SIGINT, &handlers->old_int, NULL);
        handlers->has_term = false;
        handlers->has_int = false;
        return 1;
    }
    handlers->has_hup = true;

    return 0;
}

static void xargs_restore_signal_handlers(struct xargs_signal_handlers *handlers) {
    if (handlers->has_hup)
        sigaction(SIGHUP, &handlers->old_hup, NULL);
    if (handlers->has_term)
        sigaction(SIGTERM, &handlers->old_term, NULL);
    if (handlers->has_int)
        sigaction(SIGINT, &handlers->old_int, NULL);
}

int bx_xargs_main(int argc, char **argv) {
    struct xargs_main_args args;
    if (!xargs_parse_main_args(argc, argv, &args))
        return args.exit_code;

    const char *progname = args.progname;

    struct xargs_items items = {0};
    if (!xargs_read_items(args.input, progname, &args.opts, &items)) {
        xargs_free_main_args(&args);
        xargs_items_free(&items);
        return 1;
    }
    xargs_free_main_args(&args);

    struct xargs_signal_handlers handlers;
    if (xargs_install_signal_handlers(progname, &handlers) != 0) {
        xargs_items_free(&items);
        return 1;
    }

    int rc = xargs_run_batches(progname, args.command, args.command_argc,
                               &items, &args.opts,
                               &xargs_interrupt_signal);
    xargs_restore_signal_handlers(&handlers);
    xargs_items_free(&items);
    return rc;
}
