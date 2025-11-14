#ifndef SCREEN_TERMCAP_H
#define SCREEN_TERMCAP_H

#include <stdbool.h>
#include <stdio.h>

int   InitTermcap (int, int);
char *MakeTermcap (bool);
char *gettermcapstring (char *);
int   remap (int, int);
void  CheckEscape (void);
int   CreateTransTable (char *);
void  FreeTransTable (void);
void  DumpTermcap (int, FILE *);

/* global variables */

extern char screenterm[];
extern char Term[];
extern char Termcap[];
extern char *extra_incap;
extern char *extra_outcap;

#endif /* SCREEN_TERMCAP_H */
