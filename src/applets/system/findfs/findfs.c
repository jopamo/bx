#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/disk_tag.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [--] {LABEL,UUID,PARTLABEL,PARTUUID}=VALUE\n"
                 "Resolve a block device using /dev/disk links.\n", name);
}

int bx_findfs_main(int argc, char** argv) {
    if (argc == 2) {
        int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "findfs", "-h", "-V", usage);
        if (handled >= 0)
            return handled;
    }
    struct bx_diag_ctx diag = {.progname = "findfs"};
    int operand = argc > 1 && strcmp(argv[1], "--") == 0 ? 2 : 1;
    if (argc - operand != 1) {
        usage(stderr, diag.progname);
        return 2;
    }

    const char* spec = argv[operand];
    char* device;
    int result = bx_disk_tag_resolve(spec, &device);
    if (result == 0) {
        bx_diag(&diag, "unsupported device tag: '%s'", spec);
        return 2;
    }
    if (result < 0) {
        bx_diag(&diag, "cannot resolve '%s': %s", spec, bx_strerror(errno));
        return 1;
    }
    result = puts(device);
    free(device);
    if (result == EOF || fflush(stdout) == EOF) {
        bx_perror_path(&diag, "standard output");
        return 1;
    }
    return 0;
}
