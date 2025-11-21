#ifndef SCREEN_PROCESS_PARSE_H
#define SCREEN_PROCESS_PARSE_H

#include <stdbool.h>

#include "comm.h"

int MsgOk(void);
int ParseSwitch(struct action *act, bool *var);
int ParseOnOffMapped(struct action *act, int *var, int off_value, int on_value);
int ParseOnOff(struct action *act, bool *var);
int ParseSaveStr(struct action *act, char **var);
int ParseNum(struct action *act, int *var);
int ParseNum1000(struct action *act, int *var);
int ParseWinNum(struct action *act, int *var);
int ParseBase(struct action *act, char *p, int *var, int base, char *bname);
bool IsNum(char *s);

#endif
