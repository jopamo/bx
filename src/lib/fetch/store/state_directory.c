#define _GNU_SOURCE
#include "lib/fetch/state_directory.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

char* bx_fetch_state_directory(void) {
    const char* state = getenv("XDG_STATE_HOME");
    const char* home = getenv("HOME");
    char* path = NULL;
    int rc;
    if (state && *state)
        rc = asprintf(&path, "%s/mira", state);
    else if (home && *home)
        rc = asprintf(&path, "%s/.local/state/mira", home);
    else
        rc = asprintf(&path, "/tmp/mira-%lu", (unsigned long)getuid());
    return rc < 0 ? NULL : path;
}
