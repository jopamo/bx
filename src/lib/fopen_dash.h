#ifndef BX_LIB_FOPEN_DASH_H
#define BX_LIB_FOPEN_DASH_H

#include <stdbool.h>
#include <stdio.h>

FILE *bx_fopen_dash(const char *path, const char *mode, bool *is_stdio);
void bx_fclose_nonstdio(FILE *fp, bool is_stdio);

#endif
