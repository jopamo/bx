#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include "applets/text/edit/bx_vim_config.h"
#include "applets/text/edit/bx_vim_runtime.h"

static char* bx_vim_strdup(const char* text) {
    size_t len = strlen(text);
    if (len == SIZE_MAX) {
        return NULL;
    }

    char* out = malloc(len + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, text, len + 1u);
    return out;
}

static bool bx_vim_runtime_has_defaults(const char* runtime_dir, bool* oom_out) {
    size_t dir_len = strlen(runtime_dir);
    static const char suffix[] = "/defaults.vim";
    if (dir_len > SIZE_MAX - sizeof(suffix)) {
        *oom_out = true;
        return false;
    }

    char* path = malloc(dir_len + sizeof(suffix));
    if (path == NULL) {
        *oom_out = true;
        return false;
    }
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
            if (extra == SIZE_MAX) {
                return NULL;
            }
            extra += 1;
        }
    }

    size_t len = strlen(value);
    if (len > SIZE_MAX - extra - 1u) {
        return NULL;
    }
    char* quoted = malloc(len + extra + 1u);
    if (quoted == NULL) {
        return NULL;
    }
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
    bool oom = false;
    if (bx_vim_runtime_has_defaults(BX_EDIT_VIM_RUNTIME_SOURCE_DIR, &oom)) {
        return bx_vim_strdup(BX_EDIT_VIM_RUNTIME_SOURCE_DIR);
    }
    if (oom) {
        return NULL;
    }
    return bx_vim_strdup(BX_EDIT_VIM_RUNTIME_INSTALL_DIR);
}

char* bx_vim_runtime_make_runtime_cmd(const char* runtime_dir) {
    char* quoted = bx_vim_quote_string(runtime_dir);
    if (quoted == NULL) {
        return NULL;
    }
    int len = snprintf(NULL, 0,
        "let &runtimepath='%1$s' | let &packpath=''",
        quoted);
    if (len < 0) {
        free(quoted);
        return NULL;
    }
    char* cmd = malloc((size_t)len + 1u);
    if (cmd == NULL) {
        free(quoted);
        return NULL;
    }
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
