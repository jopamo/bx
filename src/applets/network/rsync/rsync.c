/* Compile Apple's main as a bx applet, retaining its native argument parser. */
#include "openrsync-feature-config.h"

#define BX_RSYNC_APPLET 1

int bx_rsync_main(int argc, char **argv);

#define main bx_rsync_main
#include "../../../vendor/openrsync/main.c"
#undef main
