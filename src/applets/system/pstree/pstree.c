#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_pstree_reference_main(int argc, char** argv);

static void bx_pstree_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-acglpsStTuZ] [-h | -H PID] [-n | -N TYPE]\n", progname);
    fprintf(stream, "       %s [-A | -G | -U] [PID | USER]\n", progname);
    fprintf(stream, "Display processes as a tree.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --arguments         show command line arguments\n");
    fprintf(stream, "  -A, --ascii             use ASCII line drawing characters\n");
    fprintf(stream, "  -c, --compact-not       do not merge identical subtrees\n");
    fprintf(stream, "  -C, --color=TYPE        color by attribute (age)\n");
    fprintf(stream, "  -g, --show-pgids        show process group IDs\n");
    fprintf(stream, "  -G, --vt100             use VT100 line drawing characters\n");
    fprintf(stream, "  -h, --highlight-all     highlight the current process and ancestors\n");
    fprintf(stream, "  -H, --highlight-pid PID highlight PID and its ancestors\n");
    fprintf(stream, "  -k, --kthreads          show kernel threads\n");
    fprintf(stream, "  -l, --long              do not truncate long lines\n");
    fprintf(stream, "  -n, --numeric-sort      sort output by PID\n");
    fprintf(stream, "  -N, --ns-sort TYPE      sort by namespace type\n");
    fprintf(stream, "  -p, --show-pids         show PIDs\n");
    fprintf(stream, "  -P, --show-paths        show the full path for the selected process\n");
    fprintf(stream, "  -s, --show-parents      show parents of the selected process\n");
    fprintf(stream, "  -S, --ns-changes        show namespace transitions\n");
    fprintf(stream, "  -t, --thread-names      show full thread names\n");
    fprintf(stream, "  -T, --hide-threads      hide threads\n");
    fprintf(stream, "  -u, --uid-changes       show UID transitions\n");
    fprintf(stream, "  -U, --unicode           use UTF-8 line drawing characters\n");
    fprintf(stream, "  -Z, --security-context  show security attributes\n");
    fprintf(stream, "      --help              display this help and exit\n");
    fprintf(stream, "  -V, --version           output version information and exit\n");
}

int bx_pstree_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "pstree", NULL, bx_pstree_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_pstree_reference_main(argc, argv);
}
