#ifndef SCREEN_PROCESS_WINDOW_NAV_H
#define SCREEN_PROCESS_WINDOW_NAV_H

#include "screen.h"

Window *NextWindow(void);
Window *PreviousWindow(void);
Window *ParentWindow(void);
int MoreWindows(void);

#endif
