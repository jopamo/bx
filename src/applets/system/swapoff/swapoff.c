#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/swap.h>

#include "applets.h"
#include "lib/cli_common.h"
#include "lib/mount_table.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s -a\nDisable every active swap area listed in /proc/swaps.\n", name);
}

int bx_swapoff_main(int argc, char** argv) {
    int help = bx_cli_maybe_handle_help_or_version(argc, argv, "swapoff", NULL, NULL, usage);
    if (help >= 0)
        return help;
    if (argc != 2 || strcmp(argv[1], "-a")) {
        usage(stderr, "swapoff");
        return 1;
    }
    FILE* file = fopen("/proc/swaps", "re");
    if (!file) {
        perror("swapoff: /proc/swaps");
        return 1;
    }
    /* Snapshot before disabling anything: seq_file positions count active entries. */
    char* paths[32]; /* Linux MAX_SWAPFILES */
    size_t count = 0;
    char line[4 * PATH_MAX + 128];
    bool valid = fgets(line, sizeof(line), file) && strncmp(line, "Filename\t", 9) == 0 && strchr(line, '\n');
    while (valid && fgets(line, sizeof(line), file)) {
        size_t n = strcspn(line, " \t");
        if (line[0] != '/' || !line[n] || !strchr(line, '\n') || count == sizeof(paths) / sizeof(paths[0])) {
            valid = false;
            break;
        }
        line[n] = '\0';
        paths[count++] = bx_mount_unescape_field(line);
    }
    if (ferror(file))
        valid = false;
    fclose(file);
    int status = !valid;
    if (!valid)
        fprintf(stderr, "swapoff: cannot read active swaps from /proc/swaps\n");
    while (count) {
        char* path = paths[--count];
        if (valid && swapoff(path) < 0) {
            fprintf(stderr, "swapoff: %s: %s\n", path, strerror(errno));
            status = 1;
        }
        free(path);
    }
    return status;
}
