#include "search/search.h"
#include "applets.h"
#include "lib/cli_common.h"
#include <string.h>

int bx_grep_main(int argc, char **argv) {
    const char *progname = bx_cli_progname(argv[0], "grep");

    if (strcmp(progname, "egrep") == 0)
        return bx_search_main(argc, argv, BX_SEARCH_EGREP);
    if (strcmp(progname, "fgrep") == 0)
        return bx_search_main(argc, argv, BX_SEARCH_FGREP);
    return bx_search_main(argc, argv, BX_SEARCH_GREP);
}
