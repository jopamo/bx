#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lib/signal_names.h"

struct bx_signal_name_entry {
    int number;
    const char* name;
};

static const struct bx_signal_name_entry bx_signal_names[] = {
#ifdef SIGHUP
    {SIGHUP, "HUP"},
#endif
#ifdef SIGINT
    {SIGINT, "INT"},
#endif
#ifdef SIGQUIT
    {SIGQUIT, "QUIT"},
#endif
#ifdef SIGILL
    {SIGILL, "ILL"},
#endif
#ifdef SIGTRAP
    {SIGTRAP, "TRAP"},
#endif
#ifdef SIGABRT
    {SIGABRT, "ABRT"},
#endif
#ifdef SIGIOT
    {SIGIOT, "IOT"},
#endif
#ifdef SIGBUS
    {SIGBUS, "BUS"},
#endif
#ifdef SIGFPE
    {SIGFPE, "FPE"},
#endif
#ifdef SIGKILL
    {SIGKILL, "KILL"},
#endif
#ifdef SIGUSR1
    {SIGUSR1, "USR1"},
#endif
#ifdef SIGSEGV
    {SIGSEGV, "SEGV"},
#endif
#ifdef SIGUSR2
    {SIGUSR2, "USR2"},
#endif
#ifdef SIGPIPE
    {SIGPIPE, "PIPE"},
#endif
#ifdef SIGALRM
    {SIGALRM, "ALRM"},
#endif
#ifdef SIGTERM
    {SIGTERM, "TERM"},
#endif
#ifdef SIGSTKFLT
    {SIGSTKFLT, "STKFLT"},
#endif
#ifdef SIGCHLD
    {SIGCHLD, "CHLD"},
#endif
#ifdef SIGCLD
    {SIGCLD, "CLD"},
#endif
#ifdef SIGCONT
    {SIGCONT, "CONT"},
#endif
#ifdef SIGSTOP
    {SIGSTOP, "STOP"},
#endif
#ifdef SIGTSTP
    {SIGTSTP, "TSTP"},
#endif
#ifdef SIGTTIN
    {SIGTTIN, "TTIN"},
#endif
#ifdef SIGTTOU
    {SIGTTOU, "TTOU"},
#endif
#ifdef SIGURG
    {SIGURG, "URG"},
#endif
#ifdef SIGXCPU
    {SIGXCPU, "XCPU"},
#endif
#ifdef SIGXFSZ
    {SIGXFSZ, "XFSZ"},
#endif
#ifdef SIGVTALRM
    {SIGVTALRM, "VTALRM"},
#endif
#ifdef SIGPROF
    {SIGPROF, "PROF"},
#endif
#ifdef SIGWINCH
    {SIGWINCH, "WINCH"},
#endif
#ifdef SIGIO
    {SIGIO, "IO"},
#endif
#ifdef SIGPOLL
    {SIGPOLL, "POLL"},
#endif
#ifdef SIGPWR
    {SIGPWR, "PWR"},
#endif
#ifdef SIGSYS
    {SIGSYS, "SYS"},
#endif
#ifdef SIGUNUSED
    {SIGUNUSED, "UNUSED"},
#endif
#ifdef SIGEMT
    {SIGEMT, "EMT"},
#endif
#ifdef SIGINFO
    {SIGINFO, "INFO"},
#endif
};

static bool bx_signal_parse_number(const char* name, int* number_out) {
    if (name == NULL || name[0] == '\0' || !isdigit((unsigned char)name[0]) || number_out == NULL) {
        return false;
    }

    errno = 0;
    char* end = NULL;
    intmax_t value = strtoimax(name, &end, 10);
    if (errno == ERANGE || end == name || end == NULL || *end != '\0' || value > INT_MAX) {
        return false;
    }

    *number_out = (int)value;
    return true;
}

static bool bx_signal_parse_rt_offset(const char* text, int* offset_out) {
    return bx_signal_parse_number(text, offset_out);
}

static bool bx_signal_parse_realtime_name(const char* name, int* number_out) {
#if defined(SIGRTMIN) && defined(SIGRTMAX)
    int rtmin = SIGRTMIN;
    int rtmax = SIGRTMAX;
    if (strcasecmp(name, "RTMIN") == 0) {
        *number_out = rtmin;
        return true;
    }
    if (strcasecmp(name, "RTMAX") == 0) {
        *number_out = rtmax;
        return true;
    }

    int offset = 0;
    if (strncasecmp(name, "RTMIN+", 6) == 0 &&
        bx_signal_parse_rt_offset(name + 6, &offset) &&
        offset <= rtmax - rtmin) {
        *number_out = rtmin + offset;
        return true;
    }
    if (strncasecmp(name, "RTMAX-", 6) == 0 &&
        bx_signal_parse_rt_offset(name + 6, &offset) &&
        offset <= rtmax - rtmin) {
        *number_out = rtmax - offset;
        return true;
    }
#else
    (void)name;
    (void)number_out;
#endif
    return false;
}

bool bx_signal_name_lookup(const char* name, int* number_out) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    if (isdigit((unsigned char)name[0])) {
        return bx_signal_parse_number(name, number_out);
    }

    if (strncasecmp(name, "SIG", 3) == 0) {
        name += 3;
    }

    if (bx_signal_parse_realtime_name(name, number_out)) {
        return true;
    }

    for (size_t i = 0; i < (sizeof(bx_signal_names) / sizeof(bx_signal_names[0])); i++) {
        if (strcasecmp(bx_signal_names[i].name, name) == 0) {
            *number_out = bx_signal_names[i].number;
            return true;
        }
    }

    return false;
}

void bx_signal_name_list(FILE* stream) {
    int col = 0;

    for (size_t i = 0; i < (sizeof(bx_signal_names) / sizeof(bx_signal_names[0])); i++) {
        const char* name = bx_signal_names[i].name;
        size_t len = strlen(name);

        if (col + (int)len + 1 > 80) {
            fputc('\n', stream);
            col = 0;
        }

        fprintf(stream, "%s%s", col ? " " : "", name);
        col += (int)len + 1;
    }

    fputc('\n', stream);
}
