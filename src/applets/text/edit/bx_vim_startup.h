#ifndef BX_APPLETS_TEXT_EDIT_BX_VIM_STARTUP_H
#define BX_APPLETS_TEXT_EDIT_BX_VIM_STARTUP_H

struct bx_vim_invocation {
    int argc;
    char** argv;
    char* runtime_dir;
    char* runtime_cmd;
    char* policy_cmd;
    char* defaults_cmd;
};

void bx_vim_print_help(const char* progname);
int bx_vim_prepare_invocation(struct bx_vim_invocation* invocation, int argc, char** argv);
void bx_vim_free_invocation(struct bx_vim_invocation* invocation);

#endif /* BX_APPLETS_TEXT_EDIT_BX_VIM_STARTUP_H */
