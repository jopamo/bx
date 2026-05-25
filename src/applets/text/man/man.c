#include <stdio.h>

#include "dispatch/applets.h"
#include "lib/manual_runtime.h"

static void bx_man_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [OPTION]... [[-s] section] name ...\n", progname);
    fprintf(stream, "Display manual pages.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Search and selection:\n");
    fprintf(stream, "  -a                     display all matching manual pages\n");
    fprintf(stream, "  -l                     interpret operands as local manual files\n");
    fprintf(stream, "  -M path                override the manual search path\n");
    fprintf(stream, "  -m path                prepend additional manual search paths\n");
    fprintf(stream, "  -S subsection          restrict matches to the given subsection or arch\n");
    fprintf(stream, "  -s section             restrict matches to the given manual section\n");
    fprintf(stream, "\n");
    fprintf(stream, "Output control:\n");
    fprintf(stream, "  -c                     write directly to standard output without a pager\n");
    fprintf(stream, "  -h                     display only SYNOPSIS sections\n");
    fprintf(stream, "  -w                     print matching manual file paths\n");
    fprintf(stream, "\n");
    fprintf(stream, "Configuration and formatting:\n");
    fprintf(stream, "  -C file                use an alternate man.conf file\n");
    fprintf(stream, "  -I os=name             override the displayed operating system name\n");
    fprintf(stream, "  -O options             pass formatter output options\n");
    fprintf(stream, "  -T output              select output type (ascii, locale, utf8, man, markdown)\n");
    fprintf(stream, "  -W level               set warning or compatibility policy\n");
    fprintf(stream, "\n");
    fprintf(stream, "  --help                 display this help and exit\n");
    fprintf(stream, "  --version              output version information and exit\n");
}

int bx_man_main(int argc, char **argv) {
    return bx_manual_runtime_main(argc, argv, "man", bx_man_print_help);
}
