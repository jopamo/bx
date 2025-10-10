#ifndef BX_APPLETS_BASE_XARGS_PARSE_H
#define BX_APPLETS_BASE_XARGS_PARSE_H

#include <stdbool.h>
#include <stdio.h>

struct xargs_opts {
    bool no_run_if_empty;
    bool nul_delim;
    bool delimiter_mode;
    bool exit_if_too_big;
    bool open_tty;
    bool interactive;
    bool verbose;
    char delimiter;
    int max_args;
    int max_lines;
    int max_chars;
    int max_procs;
    const char *arg_file;
    const char *logical_eof;
    bool replace_mode;
    const char *replace_marker;
    const char *process_slot_var;
};

struct xargs_main_args {
    const char *progname;
    struct xargs_opts opts;
    char **command;
    int command_argc;
    FILE *input;
    bool close_input;
    int exit_code;
};

bool xargs_parse_main_args(int argc, char **argv, struct xargs_main_args *out);
void xargs_free_main_args(struct xargs_main_args *args);

#endif
