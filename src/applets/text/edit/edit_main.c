#include <string.h>
#include "applets/text/edit/bx_vim_startup.h"

int bx_vim_main(int argc, char** argv);
int bx_vim_main_impl(int argc, char** argv);

int bx_vim_main(int argc, char** argv) {
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        bx_vim_print_help(argv[0]);
        return 0;
    }

    struct bx_vim_invocation invocation;
    int rc = bx_vim_prepare_invocation(&invocation, argc, argv);
    if (rc != 0) {
        bx_vim_free_invocation(&invocation);
        return rc;
    }

    rc = bx_vim_main_impl(invocation.argc, invocation.argv);
    bx_vim_free_invocation(&invocation);
    return rc;
}
