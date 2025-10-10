#ifndef BX_APPLETS_TEXT_FD_PARSE_H
#define BX_APPLETS_TEXT_FD_PARSE_H

#include <stdbool.h>

#include "fd_internal.h"

struct fd_main_args {
    struct fd_opts opts;
    const char *progname;
    bool using_implicit_root;
    char **search_paths;
    int search_path_count;
    const char **exec_argv_storage;
    int exit_code;
};

bool fd_parse_main_args(int argc, char **argv, struct fd_main_args *out);
void fd_free_main_args(struct fd_main_args *args);

#endif
