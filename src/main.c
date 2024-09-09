#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "which.h"
#include "applets.h"
#include "diag.h"
#include "libbx.h"

typedef int (*applet_main_t)(int argc, char **argv);

struct applet {
    const char *name;
    applet_main_t main;
};

static const struct applet applets[] = {
    {"which", bx_which_main},
    {"cp", bx_cp_main},
    {"true", bx_true_main},
    {"false", bx_false_main},
};

static const char shebang_applet_prefix[] = "--bx-applet-shebang=";

static const char *get_basename(const char *path) {
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

static applet_main_t find_applet(const char *name) {
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        if (strcmp(applets[i].name, name) == 0) {
            return applets[i].main;
        }
    }
    return NULL;
}

static const char *get_shebang_applet(const char *arg) {
    size_t prefix_len = sizeof(shebang_applet_prefix) - 1;

    if (!arg || strncmp(arg, shebang_applet_prefix, prefix_len) != 0) {
        return NULL;
    }

    const char *name = arg + prefix_len;
    return (name[0] != '\0') ? name : NULL;
}

static int run_shebang_applet(applet_main_t applet_main, int argc, char **argv) {
    int applet_argc = argc - 2;
    char **applet_argv = xmalloc(((size_t)applet_argc + 1) * sizeof(*applet_argv));
    char *applet_argv0 = xstrdup(get_basename(argv[2]));

    applet_argv[0] = applet_argv0;
    for (int i = 1; i < applet_argc; i++) {
        applet_argv[i] = argv[i + 2];
    }
    applet_argv[applet_argc] = NULL;

    int rc = applet_main(applet_argc, applet_argv);
    free(applet_argv0);
    free(applet_argv);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 1) return 1;

    const char *progname = get_basename(argv[0]);
    applet_main_t applet_main = find_applet(progname);

    if (applet_main) {
        return applet_main(argc, argv);
    }

    const char *shebang_applet = (argc >= 2) ? get_shebang_applet(argv[1]) : NULL;
    if (shebang_applet) {
        applet_main = find_applet(shebang_applet);
        if (!applet_main) {
            bx_err("unknown applet in shebang wrapper: %s", shebang_applet);
            return 1;
        }
        if (argc < 3) {
            bx_err("invalid shebang wrapper invocation");
            return 1;
        }
        return run_shebang_applet(applet_main, argc, argv);
    }

    if (argc < 2) {
        goto usage;
    }

    if (strcmp(argv[1], "--help") == 0) {
        goto usage;
    }

    if (strcmp(argv[1], "--version") == 0) {
        printf("bx version %s\n", BX_VERSION);
        return 0;
    }

    applet_main = find_applet(argv[1]);
    if (applet_main) {
        return applet_main(argc - 1, argv + 1);
    }

    bx_err("unknown subcommand: %s", argv[1]);
    return 1;

usage:
    printf("usage: bx SUBCOMMAND [options] ...\n");
    printf("\n");
    printf("Currently supported subcommands:\n");
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        printf("  %s\n", applets[i].name);
    }
    return 0;
}
