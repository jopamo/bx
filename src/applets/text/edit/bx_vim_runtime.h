#ifndef BX_APPLETS_TEXT_EDIT_BX_VIM_RUNTIME_H
#define BX_APPLETS_TEXT_EDIT_BX_VIM_RUNTIME_H

char* bx_vim_runtime_resolve_dir(void);
char* bx_vim_runtime_make_runtime_cmd(const char* runtime_dir);
const char* bx_vim_runtime_policy_cmd(void);
const char* bx_vim_runtime_defaults_cmd(void);

#endif /* BX_APPLETS_TEXT_EDIT_BX_VIM_RUNTIME_H */
