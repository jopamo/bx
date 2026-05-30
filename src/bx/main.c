#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include "bx/diag.h"
#include "bx/libbx.h"
#include "dispatch/dispatch.h"
#include "lib/path_ops.h"
#include "lib/status.h"

static const char shebang_applet_prefix[] = "--bx-applet-shebang=";

static const char* get_shebang_applet(const char* arg) {
    size_t prefix_len = sizeof(shebang_applet_prefix) - 1;

    if (!arg || strncmp(arg, shebang_applet_prefix, prefix_len) != 0) {
        return NULL;
    }

    const char* name = arg + prefix_len;
    return (name[0] != '\0') ? name : NULL;
}

static int run_shebang_applet(bx_applet_main_t applet_main, int argc, char** argv) {
    int applet_argc = argc - 2;
    char** applet_argv = xmalloc(((size_t)applet_argc + 1) * sizeof(*applet_argv));
    char* applet_argv0 = xstrdup(bx_path_basename_ptr(argv[2]));

    applet_argv[0] = applet_argv0;
    for (int i = 1; i < applet_argc; i++) {
        applet_argv[i] = argv[i + 2];
    }
    applet_argv[applet_argc] = NULL;

    int rc = bx_status_run_applet(applet_main, applet_argc, applet_argv);
    free(applet_argv0);
    free(applet_argv);
    return rc;
}

static bool path_is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

static char* resolve_self_path(const char* argv0) {
    char proc_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", proc_path, sizeof(proc_path) - 1);
    if (len >= 0) {
        proc_path[len] = '\0';
        return xstrdup(proc_path);
    }

    return xstrdup(argv0 != NULL ? argv0 : "bx");
}

static int install_one_applet_shortcut(
    const char* bx_path,
    const char* install_dir,
    const char* applet_name,
    bool symlink_mode,
    struct bx_diag_ctx* diag
) {
    char* destination_path = bx_path_join(install_dir, applet_name);

    struct stat st;
    if (lstat(destination_path, &st) == 0) {
        free(destination_path);
        return bx_status_success();
    }
    if (errno != ENOENT) {
        bx_perror_path(diag, destination_path);
        free(destination_path);
        return bx_status_error();
    }

    int rc;
    if (symlink_mode) {
        rc = symlink(bx_path, destination_path);
    }
    else {
        rc = link(bx_path, destination_path);
    }

    if (rc != 0) {
        bx_perror_path(diag, destination_path);
        free(destination_path);
        return bx_status_error();
    }

    free(destination_path);
    return bx_status_success();
}

static bool applet_shortcut_install_supported(const char* applet_name) {
    (void)applet_name;
#if !BX_HAVE_MIRA_EMBED
    if (strcmp(applet_name, "wget") == 0) {
        return false;
    }
#endif
    return true;
}

static int install_missing_applets(const char* bx_path, const char* install_dir, bool symlink_mode, struct bx_diag_ctx* diag) {
    if (!path_is_directory(install_dir)) {
        bx_diag(diag, "install target is not a directory: '%s'", install_dir);
        return bx_status_error();
    }

    int status = bx_status_success();
    for (size_t i = 0; i < bx_dispatch_count(); i++) {
        const struct bx_dispatch_entry* entry = bx_dispatch_at(i);
        if (entry == NULL) {
            continue;
        }
        if (!applet_shortcut_install_supported(entry->name)) {
            continue;
        }
        if (install_one_applet_shortcut(bx_path, install_dir, entry->name, symlink_mode, diag) != 0) {
            status = bx_status_error();
        }
    }

    return status;
}

static int run_install_mode(int argc, char** argv, struct bx_diag_ctx* diag) {
    bool symlink_mode = false;
    const char* install_dir = ".";
    bool install_dir_set = false;

    for (int i = 2; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0) {
            printf("usage: bx --install [-s|--symlink] [DIR]\n");
            printf("Install missing applet shortcuts into DIR (default: .).\n");
            printf("By default hard links are created; -s creates symlinks.\n");
            return bx_status_success();
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--symlink") == 0) {
            symlink_mode = true;
            continue;
        }
        if (arg[0] == '-') {
            bx_diag(diag, "unknown --install option '%s'", arg);
            return bx_status_error();
        }
        if (install_dir_set) {
            bx_diag(diag, "unexpected --install operand '%s'", arg);
            return bx_status_error();
        }
        install_dir = arg;
        install_dir_set = true;
    }

    char* bx_path = resolve_self_path(argv[0]);
    int rc = install_missing_applets(bx_path, install_dir, symlink_mode, diag);
    free(bx_path);
    return bx_status_from_applet(rc);
}

int main(int argc, char** argv) {
    if (argc < 1)
        return bx_status_error();

    struct bx_diag_ctx diag = {
        .progname = "bx",
    };

    const char* progname = bx_path_basename_ptr(argv[0]);
    bx_applet_main_t applet_main = bx_dispatch_find(progname);
    if (!applet_main && progname[0] == '-' && progname[1] != '\0') {
        applet_main = bx_dispatch_find(progname + 1);
    }

    if (applet_main) {
        return bx_status_run_applet(applet_main, argc, argv);
    }

    const char* shebang_applet = (argc >= 2) ? get_shebang_applet(argv[1]) : NULL;
    if (shebang_applet) {
        applet_main = bx_dispatch_find(shebang_applet);
        if (!applet_main) {
            bx_diag(&diag, "unknown applet in shebang wrapper: '%s'", shebang_applet);
            return bx_status_error();
        }
        if (argc < 3) {
            bx_diag(&diag, "invalid shebang wrapper invocation");
            return bx_status_error();
        }
        return run_shebang_applet(applet_main, argc, argv);
    }

    if (argc < 2) {
        goto usage;
    }

    if (strcmp(argv[1], "--help") == 0) {
        goto usage;
    }

    if (strcmp(argv[1], "--install") == 0) {
        return bx_status_from_applet(run_install_mode(argc, argv, &diag));
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("bx version %s\n", BX_VERSION);
        return bx_status_success();
    }

    if (argv[1][0] == '-') {
        bx_diag(&diag, "unknown option '%s'", argv[1]);
        return bx_status_error();
    }

    applet_main = bx_dispatch_find(argv[1]);
    if (applet_main) {
        return bx_status_run_applet(applet_main, argc - 1, argv + 1);
    }

    bx_diag(&diag, "unknown subcommand '%s'", argv[1]);
    return bx_status_error();

usage:
    printf("usage: bx SUBCOMMAND [options] ...\n");
    printf("       bx --install [-s|--symlink] [DIR]\n");
    printf("\n");
    printf("--install creates shortcuts only for applets missing in DIR.\n");
    printf("Use -s to create symlinks (default is hard links).\n");
    printf("\n");
    printf("Currently supported subcommands:\n");
    for (size_t i = 0; i < bx_dispatch_count(); i++) {
        const struct bx_dispatch_entry* entry = bx_dispatch_at(i);
        if (entry != NULL) {
            printf("  %s\n", entry->name);
        }
    }
    return bx_status_success();
}
