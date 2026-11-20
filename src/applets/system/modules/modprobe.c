#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s --show-depends [-d ROOT] [-S RELEASE] MODULE\n"
                 "Show the hard dependency order for an exact module name.\n"
                 "Does not load modules or read modprobe configuration.\n", name);
}

#ifdef __linux__
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "module_plan.h"
#endif

int bx_modprobe_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "modprobe", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "modprobe"};
#ifdef __linux__
    static const struct option opts[] = {
        {"show-depends", no_argument, NULL, 'D'},
        {"dirname", required_argument, NULL, 'd'},
        {"set-version", required_argument, NULL, 'S'},
        {NULL, 0, NULL, 0},
    };
    const char* base = "/";
    const char* release = NULL;
    bool show = false;
    opterr = 0;
    optind = 1;
    for (int opt; (opt = getopt_long(argc, argv, ":Dd:S:", opts, NULL)) != -1;) {
        switch (opt) {
        case 'D':
            show = true;
            break;
        case 'd':
            if (*optarg == '\0' || strpbrk(optarg, "\r\n") != NULL) {
                bx_diag(&diag, "invalid module root directory");
                return 1;
            }
            base = optarg;
            break;
        case 'S':
            if (!bx_module_release_valid(optarg)) {
                bx_diag(&diag, "invalid kernel release '%s'", optarg);
                return 1;
            }
            release = optarg;
            break;
        default:
            if (opt == ':')
                bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            else
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (!show) {
        bx_diag(&diag, "only --show-depends is supported; no modules were loaded");
        return 1;
    }
    if (argc - optind != 1 || !bx_module_name_valid(argv[optind])) {
        usage(stderr, diag.progname);
        return 1;
    }
    const char* name = argv[optind];
    struct bx_module_tree tree = {.fd = -1};
    int status = 1;
    if (bx_module_tree_open(&tree, base, release) < 0) {
        bx_perror_path(&diag, tree.path ? tree.path : "module tree");
        goto out;
    }
    struct bx_module_plan plan;
    int planned = bx_module_tree_plan(&tree, name, &plan);
    if (planned < 0) {
        bx_diag(&diag, "cannot plan dependencies for '%s': %s", name,
                planned == BX_MODULE_PLAN_CYCLE ? "dependency cycle" : strerror(errno));
        goto out;
    }
    bool printed = true;
    if (plan.builtin) {
        char normalized[NAME_MAX + 1];
        size_t i;
        for (i = 0; name[i] != '\0'; i++)
            normalized[i] = name[i] == '-' ? '_' : name[i];
        normalized[i] = '\0';
        printed = printf("builtin %s\n", normalized) >= 0;
    }
    for (size_t i = 0; printed && i < plan.count; i++) {
        struct bx_module_path path = plan.files[i];
        printed = printf("insmod %s/%.*s\n", tree.path, (int)path.len, path.text) >= 0;
    }
    free(plan.files);
    if (!printed || fflush(stdout) == EOF)
        bx_perror_path(&diag, "standard output");
    else
        status = 0;
out:
    bx_module_tree_close(&tree);
    return status;
#else
    bx_diag(&diag, "Linux module dependency queries are not supported on this platform");
    return 1;
#endif
}
