#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_killall_reference_main(int argc, char** argv);

static void bx_killall_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [--] NAME...\n", progname);
    fprintf(stream, "       %s -l\n", progname);
    fprintf(stream, "Send signals to processes selected by name.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -e, --exact            require exact match for long names\n");
    fprintf(stream, "  -g, --process-group    signal the process group instead of each PID\n");
    fprintf(stream, "  -i, --interactive      ask before signaling each match\n");
    fprintf(stream, "  -I, --ignore-case      match process names case-insensitively\n");
    fprintf(stream, "  -l, --list             list known signal names\n");
    fprintf(stream, "  -n, --ns PID           match processes in PID's namespaces\n");
    fprintf(stream, "  -o, --older-than TIME  match processes older than TIME\n");
    fprintf(stream, "  -q, --quiet            suppress complaints about missing matches\n");
    fprintf(stream, "  -r, --regexp           treat NAME as an extended regular expression\n");
    fprintf(stream, "  -s, --signal SIGNAL    send SIGNAL instead of TERM\n");
    fprintf(stream, "  -u, --user USER        match only processes owned by USER\n");
    fprintf(stream, "  -v, --verbose          report each successful signal\n");
    fprintf(stream, "  -w, --wait             wait for matched processes to exit\n");
    fprintf(stream, "  -y, --younger-than TIME\n");
    fprintf(stream, "                        match processes younger than TIME\n");
    fprintf(stream, "  -Z, --context REGEXP   match only processes with a matching context\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

int bx_killall_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "killall", "-h", bx_killall_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_killall_reference_main(argc, argv);
}
