#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"

int bx_lsmod_main(int argc, char** argv) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        puts("usage: lsmod\nList loaded Linux modules.");
        return 0;
    }

    struct bx_diag_ctx diag = {.progname = "lsmod"};
    if (argc > 1 && !(argc == 2 && strcmp(argv[1], "--") == 0)) {
        bx_diag(&diag, "unexpected argument: '%s'", strcmp(argv[1], "--") == 0 ? argv[2] : argv[1]);
        return 1;
    }
#ifdef __linux__
    const char* path = "/proc/modules";
    FILE* modules = fopen(path, "r");
    if (modules == NULL) {
        bx_perror_path(&diag, path);
        return 1;
    }

    int status = 0;
    char* line = NULL;
    size_t capacity = 0;
    if (puts("Module                  Size  Used by") == EOF)
        goto output_error;
    for (;;) {
        if (getline(&line, &capacity, modules) < 0) {
            if (ferror(modules) || !feof(modules)) {
                bx_perror_path(&diag, path);
                status = 1;
            }
            break;
        }
        const char* separators = " \t\n";
        char* next;
        char* fields[4];
        for (size_t i = 0; i < 4; i++) {
            fields[i] = strtok_r(i == 0 ? line : NULL, separators, &next);
            if (fields[i] == NULL) {
                bx_diag(&diag, "%s: incomplete module record", path);
                status = 1;
                goto flush;
            }
        }
        /* The proc entry separates users with commas and ends the list with one. */
        char* users = fields[3];
        size_t len = strlen(users);
        if (users[len - 1] == ',')
            users[len - 1] = '\0';
        if (strcmp(users, "-") == 0)
            users[0] = '\0';
        if (printf("%-19s %8s  %s%s%s\n", fields[0], fields[1], fields[2], *users ? " " : "", users) < 0)
            goto output_error;
    }
flush:
    if (fflush(stdout) == EOF)
        goto output_error;
    goto done;

output_error:
    bx_perror_path(&diag, "standard output");
    status = 1;
done:
    free(line);
    fclose(modules);
    return status;
#else
    bx_diag(&diag, "Linux module listing is not supported on this platform");
    return 1;
#endif
}
