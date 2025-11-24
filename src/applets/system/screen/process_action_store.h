#ifndef SCREEN_PROCESS_ACTION_STORE_H
#define SCREEN_PROCESS_ACTION_STORE_H

#include "comm.h"

struct action *FindKtab(char *class, int create);
void ClearAction(struct action *act);
void SaveAction(struct action *act, int nr, char **args, int *argl);
char **SaveArgs(char **args);

#endif
