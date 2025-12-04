#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "prompt_ops.h"

bool bx_prompt_confirm(const char* prompt) {
    if (!isatty(STDIN_FILENO)) {
        // GNU tools usually don't prompt if stdin is not a tty
        // but it depends on the applet.
        // For now, assume if not a TTY, we read from stdin.
    }

    fprintf(stderr, "%s", prompt);
    fflush(stderr);

    char* line = NULL;
    size_t len = 0;
    if (getline(&line, &len, stdin) == -1) {
        free(line);
        return false;
    }

    char* p = line;
    while (isspace((unsigned char)*p))
        p++;

    bool confirmed = (tolower((unsigned char)*p) == 'y');
    free(line);
    return confirmed;
}
