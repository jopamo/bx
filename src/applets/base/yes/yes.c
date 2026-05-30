#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "applets.h"
#include "lib/line_writer.h"

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
    size_t output_len = 1u;

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
        output_len = strlen(output);
    }

    char output_buffer[8192];
    struct bx_line_writer writer;
    bx_line_writer_init(&writer, STDOUT_FILENO, output_buffer, sizeof(output_buffer));

    while (bx_line_writer_put_line_len(&writer, output, output_len)) {
        ;
    }

    free(buffer);
    return 1;
}
