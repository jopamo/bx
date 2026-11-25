#include <stdio.h>

#include "applets.h"
#include "applets/system/chown/chown.h"

static void bx_chgrp_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... GROUP FILE...\n", progname);
    fprintf(stream, "  or:  %s [OPTION]... --reference=RFILE FILE...\n", progname);
    fprintf(stream, "Change the group of each FILE to GROUP.\n");
    fprintf(stream, "With --reference, change the group of each FILE to that of RFILE.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -c, --changes                 report only when a change is made\n");
    fprintf(stream, "  -f, --silent, --quiet         suppress most diagnostics\n");
    fprintf(stream, "  -v, --verbose                 report each processed path\n");
    fprintf(stream, "  -h, --no-dereference          change symlink ownership itself\n");
    fprintf(stream, "      --dereference             change symlink referent (default)\n");
    fprintf(stream, "      --from=OWNER:GROUP        apply only when current owner/group match\n");
    fprintf(stream, "      --reference=RFILE         copy group from RFILE\n");
    fprintf(stream, "      --preserve-root           fail on recursive '/'\n");
    fprintf(stream, "      --no-preserve-root        allow recursive '/'\n");
    fprintf(stream, "  -R, --recursive               recurse into directories\n");
    fprintf(stream, "  -H                            follow command-line symlink dirs with -R\n");
    fprintf(stream, "  -L                            follow all symlink dirs with -R\n");
    fprintf(stream, "  -P                            follow no symlink dirs with -R (default)\n");
    fprintf(stream, "      --help                    display this help and exit\n");
    fprintf(stream, "      --version                 output version information and exit\n");
    fprintf(stream, "\n");
    fprintf(stream, "Examples:\n");
    fprintf(stream, "  %s staff /u      Change the group of /u to \"staff\".\n", progname);
    fprintf(stream, "  %s -hR staff /u  Change the group of /u and subfiles to \"staff\".\n", progname);
}

int bx_chgrp_main(int argc, char** argv) {
    return bx_chown_run(argc, argv, true, bx_chgrp_print_help);
}
