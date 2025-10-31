#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets/system/psmisc/psmisc_signals.h"

struct bx_psmisc_signal_alias {
    int number;
    const char* name;
    int primary;
};

static const struct bx_psmisc_signal_alias bx_psmisc_signal_aliases[] = {
    {SIGHUP, "HUP", 1},
    {SIGINT, "INT", 1},
    {SIGQUIT, "QUIT", 1},
    {SIGILL, "ILL", 1},
#ifdef SIGTRAP
    {SIGTRAP, "TRAP", 1},
#endif
    {SIGABRT, "ABRT", 1},
#ifdef SIGIOT
    {SIGIOT, "IOT", 0},
#endif
#ifdef SIGBUS
    {SIGBUS, "BUS", 1},
#endif
    {SIGFPE, "FPE", 1},
    {SIGKILL, "KILL", 1},
    {SIGUSR1, "USR1", 1},
    {SIGSEGV, "SEGV", 1},
    {SIGUSR2, "USR2", 1},
    {SIGPIPE, "PIPE", 1},
    {SIGALRM, "ALRM", 1},
    {SIGTERM, "TERM", 1},
#ifdef SIGSTKFLT
    {SIGSTKFLT, "STKFLT", 1},
#endif
    {SIGCHLD, "CHLD", 1},
#ifdef SIGCLD
    {SIGCLD, "CLD", 0},
#endif
    {SIGCONT, "CONT", 1},
    {SIGSTOP, "STOP", 1},
    {SIGTSTP, "TSTP", 1},
    {SIGTTIN, "TTIN", 1},
    {SIGTTOU, "TTOU", 1},
#ifdef SIGURG
    {SIGURG, "URG", 1},
#endif
#ifdef SIGXCPU
    {SIGXCPU, "XCPU", 1},
#endif
#ifdef SIGXFSZ
    {SIGXFSZ, "XFSZ", 1},
#endif
#ifdef SIGVTALRM
    {SIGVTALRM, "VTALRM", 1},
#endif
#ifdef SIGPROF
    {SIGPROF, "PROF", 1},
#endif
#ifdef SIGWINCH
    {SIGWINCH, "WINCH", 1},
#endif
#ifdef SIGIO
    {SIGIO, "IO", 1},
#endif
#ifdef SIGPOLL
    {SIGPOLL, "POLL", 0},
#endif
#ifdef SIGPWR
    {SIGPWR, "PWR", 1},
#endif
#ifdef SIGSYS
    {SIGSYS, "SYS", 1},
#ifdef SIGUNUSED
    {SIGUNUSED, "UNUSED", 0},
#endif
#endif
#ifdef SIGEMT
    {SIGEMT, "EMT", 1},
#endif
#ifdef SIGINFO
    {SIGINFO, "INFO", 1},
#endif
};

void bx_psmisc_list_signals(void) {
    int col = 0;

    for (size_t i = 0; i < sizeof(bx_psmisc_signal_aliases) / sizeof(bx_psmisc_signal_aliases[0]); i++) {
        const char* name = bx_psmisc_signal_aliases[i].name;

        if (!bx_psmisc_signal_aliases[i].primary) {
            continue;
        }

        size_t width = strlen(name);
        if (col != 0 && col + 1 + (int)width > 80) {
            putchar('\n');
            col = 0;
        }

        if (col != 0) {
            putchar(' ');
            col++;
        }

        fputs(name, stdout);
        col += (int)width;
    }

    putchar('\n');
}

int bx_psmisc_get_signal(char* name, const char* cmd) {
    if (name == NULL || name[0] == '\0') {
        fprintf(stderr, "%s: unknown signal; %s -l lists signals.\n", "", cmd);
        exit(1);
    }

    if (isdigit((unsigned char)name[0])) {
        return atoi(name);
    }

    if (strncmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    for (size_t i = 0; i < sizeof(bx_psmisc_signal_aliases) / sizeof(bx_psmisc_signal_aliases[0]); i++) {
        if (strcmp(name, bx_psmisc_signal_aliases[i].name) == 0) {
            return bx_psmisc_signal_aliases[i].number;
        }
    }

    fprintf(stderr, "%s: unknown signal; %s -l lists signals.\n", name, cmd);
    exit(1);
}
