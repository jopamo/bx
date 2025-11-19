#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "fopen_dash.h"

FILE *bx_fopen_dash(const char *path, const char *mode, bool *is_stdio) {
    if (!path || !mode || mode[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }

    if (strcmp(path, "-") == 0) {
        FILE *fp = NULL;
        if (mode[0] == 'r') {
            fp = stdin;
        } else if (mode[0] == 'w' || mode[0] == 'a') {
            fp = stdout;
        } else {
            errno = EINVAL;
            return NULL;
        }
        if (is_stdio)
            *is_stdio = true;
        return fp;
    }

    if (is_stdio)
        *is_stdio = false;
    return fopen(path, mode);
}

void bx_fclose_nonstdio(FILE *fp, bool is_stdio) {
    if (!fp || is_stdio)
        return;
    fclose(fp);
}
