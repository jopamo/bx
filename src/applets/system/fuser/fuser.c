#include <stdio.h>

#include "applets.h"
#include "applets/system/psmisc/psmisc_wrapper.h"

int bx_fuser_reference_main(int argc, char** argv);

static void bx_fuser_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [-fIMuvw] [-a|-s] [-4|-6] [-c|-m|-n SPACE]\n", progname);
    fprintf(stream, "       %s [-k [-i] [-SIGNAL]] NAME...\n", progname);
    fprintf(stream, "       %s -l\n", progname);
    fprintf(stream, "Show which processes use the named files, sockets, or filesystems.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -a, --all              display unused files too\n");
    fprintf(stream, "  -c, -m, --mount        show all processes using the named filesystem\n");
    fprintf(stream, "  -f                     ignored for compatibility\n");
    fprintf(stream, "  -i, --interactive      ask before killing (with -k)\n");
    fprintf(stream, "  -I, --inode            compare by inode even through path aliases\n");
    fprintf(stream, "  -k, --kill             kill processes accessing the named file\n");
    fprintf(stream, "  -l, --list-signals     list available signal names\n");
    fprintf(stream, "  -M, --ismountpoint     require NAME to be a mount point\n");
    fprintf(stream, "  -n, --namespace SPACE  search the file, tcp, or udp namespace\n");
    fprintf(stream, "  -s, --silent           silent operation\n");
    fprintf(stream, "  -u, --user             display user IDs\n");
    fprintf(stream, "  -v, --verbose          verbose output\n");
    fprintf(stream, "  -w, --writeonly        kill only processes with write access\n");
    fprintf(stream, "  -4, --ipv4             search IPv4 sockets only\n");
    fprintf(stream, "  -6, --ipv6             search IPv6 sockets only\n");
    fprintf(stream, "  -SIGNAL                send this signal instead of KILL\n");
    fprintf(stream, "  -h, --help             display this help and exit\n");
    fprintf(stream, "  -V, --version          output version information and exit\n");
}

int bx_fuser_main(int argc, char** argv) {
    int handled = bx_psmisc_maybe_handle_help_or_version(argc, argv, "fuser", "-h", bx_fuser_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_fuser_reference_main(argc, argv);
}
