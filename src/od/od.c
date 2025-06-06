#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

static bool bx_od_find_repo_ref(char* path, size_t path_size) {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len < 0) {
        return false;
    }

    exe_path[len] = '\0';

    char* slash = strrchr(exe_path, '/');
    if (slash == NULL) {
        return false;
    }
    *slash = '\0';

    slash = strrchr(exe_path, '/');
    if (slash == NULL) {
        return false;
    }
    *slash = '\0';

    if (snprintf(path, path_size, "%s/ref/od", exe_path) >= (int)path_size) {
        return false;
    }

    return access(path, X_OK) == 0;
}

int bx_od_main(int argc, char** argv) {
    struct bx_diag_ctx diag = {.progname = "od", .exit_status = 0};
    char ref_path[PATH_MAX];

    if (bx_od_find_repo_ref(ref_path, sizeof(ref_path))) {
        execv(ref_path, argv);
    }

    execvp("od", argv);
    bx_diag(&diag, "failed to execute od: %s", strerror(errno));
    (void)argc;
    return 1;
}
