#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "applets/text/edit/bx_vim_config.h"
#include "applets/text/edit/bx_vim_runtime.h"
#include "bx/libbx.h"

static bool bx_vim_runtime_has_defaults(const char* runtime_dir) {
    size_t dir_len = strlen(runtime_dir);
    static const char suffix[] = "/defaults.vim";
    char* path = xmalloc(dir_len + sizeof(suffix));
    memcpy(path, runtime_dir, dir_len);
    memcpy(path + dir_len, suffix, sizeof(suffix));
    bool ok = access(path, R_OK) == 0;
    free(path);
    return ok;
}

static char* bx_vim_quote_string(const char* value) {
    size_t extra = 0;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == '\'') {
            extra += 1;
        }
    }

    size_t len = strlen(value);
    char* quoted = xmalloc(len + extra + 1);
    char* out = quoted;
    for (const char* p = value; *p != '\0'; ++p) {
        if (*p == '\'') {
            *out++ = '\'';
        }
        *out++ = *p;
    }
    *out = '\0';
    return quoted;
}

char* bx_vim_runtime_resolve_dir(void) {
    if (bx_vim_runtime_has_defaults(BX_EDIT_VIM_RUNTIME_SOURCE_DIR)) {
        return xstrdup(BX_EDIT_VIM_RUNTIME_SOURCE_DIR);
    }
    return xstrdup(BX_EDIT_VIM_RUNTIME_INSTALL_DIR);
}

char* bx_vim_runtime_make_runtime_cmd(const char* runtime_dir) {
    char* quoted = bx_vim_quote_string(runtime_dir);
    int len = snprintf(NULL, 0,
        "let &runtimepath='%1$s' | let &packpath=''",
        quoted);
    char* cmd = xmalloc((size_t)len + 1);
    snprintf(cmd, (size_t)len + 1,
        "let &runtimepath='%1$s' | let &packpath=''",
        quoted);
    free(quoted);
    return cmd;
}

const char* bx_vim_runtime_policy_cmd(void) {
    return "set nomodeline secure noexrc";
}

const char* bx_vim_runtime_defaults_cmd(void) {
    return "runtime defaults.vim";
}
