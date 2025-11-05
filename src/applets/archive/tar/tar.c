#include <stdio.h>

#include "applets/archive/tar/tar_backend.h"
#include "dispatch/applets.h"
#include "lib/cli_common.h"

static void bx_tar_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s -cf ARCHIVE [OPTION]... FILE...\n", progname);
    fprintf(stream, "       %s -tf ARCHIVE [OPTION]... [MEMBER...]\n", progname);
    fprintf(stream, "       %s -xf ARCHIVE [OPTION]... [MEMBER...]\n", progname);
    fprintf(stream, "Manipulate tar archives.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c                    create a new archive\n");
    fprintf(stream, "  -t                    list archive members\n");
    fprintf(stream, "  -x                    extract archive members\n");
    fprintf(stream, "  -f ARCHIVE            use ARCHIVE instead of standard input/output\n");
    fprintf(stream, "  -C DIR                change to DIR before processing files\n");
    fprintf(stream, "  -O                    write extracted file data to standard output\n");
    fprintf(stream, "  -r                    append files to an existing archive\n");
    fprintf(stream, "      --delete          remove named members from an archive\n");
    fprintf(stream, "  -z                    filter the archive through gzip\n");
    fprintf(stream, "  -a                    choose compression from the archive suffix\n");
    fprintf(stream, "      --help            display this help and exit\n");
    fprintf(stream, "      --version         output version information and exit\n");
}

int bx_tar_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "tar", NULL, NULL, bx_tar_print_help);
    if (handled >= 0) {
        return handled;
    }

    return bx_tar_run(argc, argv);
}
