#ifndef SCREEN_PROCESS_PROMPTS_H
#define SCREEN_PROCESS_PROMPTS_H

#include <stddef.h>

#include "screen.h"

void InputSelect(void);
void InputSetenv(char *arg);
void InputAKA(void);
int InputSu(struct acluser **up, char *name);
void SelectLayoutFin(char *buf, size_t len, void *data);
void copy_reg_fn(char *buf, size_t len, void *data);
void ins_reg_fn(char *buf, size_t len, void *data);
void process_fn(char *buf, size_t len, void *data);
void confirm_fn(char *buf, size_t len, void *data);

#endif
