#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "which.h"
#include "applets.h"
#include "diag.h"

typedef int (*applet_main_t)(int argc, char **argv);

struct applet {
    const char *name;
    applet_main_t main;
};

static const struct applet applets[] = {
    {"which", bx_which_main},
    {"true", bx_true_main},
    {"false", bx_false_main},
};

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

int main(int argc, char **argv) {
    if (argc < 1) return 1;

    const char *progname = get_basename(argv[0]);
    applet_main_t applet_main = find_applet(progname);

    if (applet_main) {
        return applet_main(argc, argv);
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
