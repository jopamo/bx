#ifndef SCREEN_PROCESS_RESIZE_PROMPT_H
#define SCREEN_PROCESS_RESIZE_PROMPT_H

#include <stddef.h>

#define RESIZE_FLAG_H 1
#define RESIZE_FLAG_V 2
#define RESIZE_FLAG_L 4

const char *ResizePrompt(int flags);
void ResizeRegions(char *arg, int flags);
void ResizeFin(char *buf, size_t len, void *data);

#endif
