#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "applets.h"

int bx_yes_main(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0)) {
        printf("Usage: %s [STRING]...\n", argv[0]);
        printf("Repeatedly output a line with all specified STRING(s), or 'y'.\n");
        return 0;
    }
    if (argc > 1 && (strcmp(argv[1], "--version") == 0)) {
        printf("yes (bx)\n");
        return 0;
    }

    const char* output = "y";
    char* buffer = NULL;

    if (argc > 1) {
        size_t len = 0;
        for (int i = 1; i < argc; i++) {
            len += strlen(argv[i]) + 1;
        }
        buffer = malloc(len);
        if (!buffer)
            return 1;
        buffer[0] = '\0';
        for (int i = 1; i < argc; i++) {
            strcat(buffer, argv[i]);
            if (i < argc - 1) {
                strcat(buffer, " ");
            }
        }
        output = buffer;
    }

    while (1) {
        if (printf("%s\n", output) < 0) {
            break;
        }
    }

    free(buffer);
    return 0;
}
