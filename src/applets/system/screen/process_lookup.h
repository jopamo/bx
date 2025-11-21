#ifndef SCREEN_PROCESS_LOOKUP_H
#define SCREEN_PROCESS_LOOKUP_H

#include "screen.h"

Window *WindowByName(char *s);
int WindowByNumber(char *string);
int WindowByNoN(char *string);
int IsNumColon(char *s, char *p, int psize);

#endif
