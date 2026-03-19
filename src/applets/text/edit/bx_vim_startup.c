#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "applets/text/edit/bx_vim_runtime.h"
#include "applets/text/edit/bx_vim_startup.h"
#include "bx/libbx.h"

static bool bx_vim_is_exact_version_request(int argc, char** argv) {
    return argc == 2 && strcmp(argv[1], "--version") == 0;
}

static int bx_vim_reject_unsafe_arg(const char* progname, const char* arg) {
    fprintf(stderr,
        "%s: unsupported startup option in fenced mode: %s\n"
        "%s: use only file paths, --, --help, or --version\n",
        progname,
        arg,
        progname);
    return 2;
}

void bx_vim_print_help(const char* progname) {
    printf("Usage: %s [--help] [--version] [--] [FILE]...\n", progname);
    puts("Open FILEs in bx's fenced Vim-derived editor.");
    puts("");
    puts("Allowed arguments:");
    puts("  --help       display this help and exit");
    puts("  --version    print Vim-derived version information and exit");
    puts("  --           stop option parsing; following arguments are file names");
    puts("");
    puts("Unsafe Vim startup controls are intentionally rejected:");
    puts("  -u -U -i -c --cmd -S plugins modelines runtime overrides");
}

int bx_vim_prepare_invocation(struct bx_vim_invocation* invocation, int argc, char** argv) {
    bool end_of_options = false;
    bool allow_version = bx_vim_is_exact_version_request(argc, argv);

    memset(invocation, 0, sizeof(*invocation));

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!end_of_options && strcmp(arg, "--") == 0) {
            end_of_options = true;
            continue;
        }
        if (!end_of_options && strcmp(arg, "-") == 0) {
            continue;
        }
        if (allow_version && i == 1 && strcmp(arg, "--version") == 0) {
            continue;
        }
        if (!end_of_options && (arg[0] == '-' || arg[0] == '+')) {
            return bx_vim_reject_unsafe_arg(argv[0], arg);
        }
    }

    invocation->runtime_dir = bx_vim_runtime_resolve_dir();
    invocation->runtime_cmd = bx_vim_runtime_make_runtime_cmd(invocation->runtime_dir);

    const int prefix_argc = 16;
    invocation->argc = prefix_argc + argc - 1;
    invocation->argv = xmalloc(((size_t)invocation->argc + 1) * sizeof(*invocation->argv));

    int out = 0;
    invocation->argv[out++] = argv[0];
    invocation->argv[out++] = "-Z";
    invocation->argv[out++] = "-N";
    invocation->argv[out++] = "-u";
    invocation->argv[out++] = "NONE";
    invocation->argv[out++] = "-U";
    invocation->argv[out++] = "NONE";
    invocation->argv[out++] = "-i";
    invocation->argv[out++] = "NONE";
    invocation->argv[out++] = "--noplugin";
    invocation->argv[out++] = "--cmd";
    invocation->argv[out++] = invocation->runtime_cmd;
    invocation->argv[out++] = "--cmd";
    invocation->argv[out++] = (char*)bx_vim_runtime_policy_cmd();
    invocation->argv[out++] = "--cmd";
    invocation->argv[out++] = (char*)bx_vim_runtime_defaults_cmd();

    for (int i = 1; i < argc; ++i) {
        invocation->argv[out++] = argv[i];
    }
    invocation->argv[out] = NULL;
    return 0;
}

void bx_vim_free_invocation(struct bx_vim_invocation* invocation) {
    free(invocation->argv);
    free(invocation->runtime_cmd);
    free(invocation->runtime_dir);
    invocation->argv = NULL;
    invocation->runtime_cmd = NULL;
    invocation->runtime_dir = NULL;
    invocation->argc = 0;
}
