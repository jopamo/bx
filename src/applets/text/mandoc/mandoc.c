#include <stdio.h>

#include "dispatch/applets.h"
#include "lib/manual_runtime.h"

static void bx_mandoc_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [OPTION]... [file ...]\n", progname);
    fprintf(stream, "Format manual source files.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Input and parser control:\n");
    fprintf(stream, "  -I os=name             override the displayed operating system name\n");
    fprintf(stream, "  -K encoding            set input encoding (utf-8, iso-8859-1, us-ascii)\n");
    fprintf(stream, "  -mdoc                  force mdoc input parsing\n");
    fprintf(stream, "  -man                   force man input parsing\n");
    fprintf(stream, "  -mmarkdown             parse the bx markdown subset as input\n");
    fprintf(stream, "\n");
    fprintf(stream, "Output control:\n");
    fprintf(stream, "  -O options             pass formatter output options\n");
    fprintf(stream, "  -T output              select output type (ascii, locale, utf8, man, markdown)\n");
    fprintf(stream, "  -W level               set warning or compatibility policy\n");
    fprintf(stream, "\n");
    fprintf(stream, "Compatibility switches accepted by the shared manual core:\n");
    fprintf(stream, "  -a                     accepted for compatibility; mandoc already processes all files\n");
    fprintf(stream, "  -c                     accepted for compatibility; mandoc already writes directly to stdout\n");
    fprintf(stream, "\n");
    fprintf(stream, "  --help                 display this help and exit\n");
    fprintf(stream, "  --version              output version information and exit\n");
}

int bx_mandoc_main(int argc, char **argv) {
    return bx_manual_runtime_main(argc, argv, "mandoc", bx_mandoc_print_help);
}
