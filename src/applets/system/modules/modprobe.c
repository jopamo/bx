#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s --show-depends [-d ROOT] [-S RELEASE] MODULE_OR_ALIAS\n"
                 "       %s --resolve-alias [-d ROOT] [-S RELEASE] ALIAS\n"
                 "       %s [-C FILE] MODULE_OR_ALIAS [PARAMETER ...]\n"
                 "       %s -r MODULE\n"
                 "Load by name or installed alias for the running kernel. Read modprobe.d\n"
                 "or FILE for blacklist and options directives. Also honors\n"
                 "modprobe.blacklist= on the kernel command line. No hooks run.\n"
                 "-r removes an exact name and then its unused installed hard dependencies.\n",
                 name, name, name, name);
}

#ifdef __linux__
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"
#include "module_alias.h"
#include "module_load.h"
#include "module_plan.h"
#include "module_policy.h"

static bool print_name(const char* prefix, const char* name, size_t len) {
    char normalized[NAME_MAX + 1];
    for (size_t i = 0; i < len; i++)
        normalized[i] = name[i] == '-' ? '_' : name[i];
    normalized[len] = '\0';
    return printf("%s%s\n", prefix, normalized) >= 0;
}

static int show_aliases(struct bx_module_tree* tree, const char* query, struct bx_diag_ctx* diag) {
    struct bx_module_aliases aliases;
    if (bx_module_resolve_alias(tree, query, &aliases) < 0) {
        bx_diag(diag, "cannot resolve alias '%s': %s", query, strerror(errno));
        return 1;
    }
    bool printed = true;
    for (size_t i = 0; printed && i < aliases.count; i++) {
        struct bx_module_path name = bx_module_alias_at(&aliases, i);
        printed = print_name("", name.text, name.len);
    }
    free(aliases.matches);
    if (!printed || fflush(stdout) == EOF) {
        bx_perror_path(diag, "standard output");
        return 1;
    }
    return 0;
}

static int load_plan(const struct bx_module_tree* tree, const struct bx_module_plan* plan,
                     const struct bx_module_policy* policy, const char* parameters,
                     struct bx_diag_ctx* diag) {
    enum { LOAD_LIMIT = 256 };
    size_t count = 0;
    for (size_t i = 0; i < plan->count; i++) {
        const struct bx_module_step* step = &plan->steps[i];
        if (step->builtin) {
            if (step->requested && *parameters != '\0') {
                bx_diag(diag, "cannot set parameters on a built-in module");
                return 1;
            }
            continue;
        }
        if (bx_module_policy_blocks(policy, step->name)) {
            bx_diag(diag, "module '%.*s' is blacklisted", (int)step->name.len, step->name.text);
            return 1;
        }
        if (++count > LOAD_LIMIT) {
            bx_diag(diag, "too many modules in load plan");
            return 1;
        }
    }
    struct input {
        const struct bx_module_step* step;
        struct stat st;
        int fd;
        char* args;
    };
    struct input* inputs = calloc(count ? count : 1, sizeof(*inputs));
    if (inputs == NULL) {
        bx_perror_path(diag, "module load plan");
        return 1;
    }
    int status = 1;
    size_t n = 0;
    /* Pin every file before the first load, so a path or fd error cannot
     * leave behind a partial load and the syscall gets the checked fd. */
    for (size_t i = 0; i < plan->count; i++) {
        const struct bx_module_step* step = &plan->steps[i];
        if (step->builtin)
            continue;
        struct bx_module_path target = step->target;
        char path[PATH_MAX];
        memcpy(path, target.text, target.len);
        path[target.len] = '\0';
        int fd = bx_module_tree_open_for_load(tree, path);
        if (fd < 0) {
            bx_perror_path(diag, path);
            goto out;
        }
        struct input* input = &inputs[n++];
        input->step = step;
        input->fd = fd;
        if (fstat(fd, &input->st) < 0) {
            bx_perror_path(diag, path);
            goto out;
        }
        if (!S_ISREG(input->st.st_mode)) {
            bx_diag(diag, "module is not a regular file: '%s'", path);
            goto out;
        }
        input->args = bx_module_policy_parameters(policy, step->name,
                                                   step->requested ? parameters : "");
        if (input->args == NULL) {
            bx_perror_path(diag, "module parameters");
            goto out;
        }
    }
    for (size_t i = 0; i < n; i++) {
        const struct input* input = &inputs[i];
        if (bx_module_load_fd(input->fd, &input->st, input->args) < 0 && errno != EEXIST) {
            struct bx_module_path target = input->step->target;
            bx_diag(diag, "cannot load '%.*s': %s", (int)target.len, target.text, strerror(errno));
            goto out;
        }
    }
    status = 0;
out:
    for (size_t i = 0; i < n; i++) {
        bx_fd_cleanup(&inputs[i].fd);
        free(inputs[i].args);
    }
    free(inputs);
    return status;
}

static int remove_plan(const struct bx_module_plan* plan, struct bx_diag_ctx* diag) {
    /* A single exact request is last in a plan of hard dependencies. */
    const struct bx_module_step* root = &plan->steps[plan->count - 1];
    if (root->builtin) {
        bx_diag(diag, "module '%.*s' is built in", (int)root->name.len, root->name.text);
        return 1;
    }
    for (size_t i = 0; i < plan->count; i++) {
        struct bx_module_path name = plan->steps[i].name;
        if (!bx_module_remove_name_valid(name)) {
            bx_diag(diag, "invalid module name: '%.*s'", (int)name.len, name.text);
            return 1;
        }
    }
    if (bx_module_remove(root->name) < 0) {
        int error = errno;
        if (error == ENOENT)
            return 0; /* Do not prune the dependencies of an absent module. */
        bx_module_remove_report(diag, root->name, error);
        return 1;
    }
    for (size_t i = plan->count - 1; i > 0; i--) {
        struct bx_module_path name = plan->steps[i - 1].name;
        if (bx_module_remove(name) < 0) {
            int error = errno;
            if (error != ENOENT && error != EWOULDBLOCK && error != EBUSY) {
                bx_module_remove_report(diag, name, error);
                return 1;
            }
        }
    }
    return 0;
}
#endif

int bx_modprobe_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "modprobe", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "modprobe"};
#ifdef __linux__
    static const struct option opts[] = {
        {"show-depends", no_argument, NULL, 'D'},
        {"resolve-alias", no_argument, NULL, 'R'},
        {"remove", no_argument, NULL, 'r'},
        {"dirname", required_argument, NULL, 'd'},
        {"set-version", required_argument, NULL, 'S'},
        {"config", required_argument, NULL, 'C'},
        {NULL, 0, NULL, 0},
    };
    const char* base = "/";
    const char* release = NULL;
    const char* config = NULL;
    bool show = false;
    bool alias = false;
    bool remove = false;
    opterr = 0;
    optind = 1;
    bool alternate_root = false;
    for (int opt; (opt = getopt_long(argc, argv, ":DRrd:S:C:", opts, NULL)) != -1;) {
        switch (opt) {
        case 'D':
            show = true;
            break;
        case 'R':
            alias = true;
            break;
        case 'r':
            remove = true;
            break;
        case 'd':
            if (*optarg == '\0' || strpbrk(optarg, "\r\n") != NULL) {
                bx_diag(&diag, "invalid module root directory");
                return 1;
            }
            base = optarg;
            alternate_root = true;
            break;
        case 'S':
            if (!bx_module_release_valid(optarg)) {
                bx_diag(&diag, "invalid kernel release '%s'", optarg);
                return 1;
            }
            release = optarg;
            break;
        case 'C':
            if (*optarg == '\0' || config != NULL) {
                bx_diag(&diag, "specify one non-empty policy filename");
                return 1;
            }
            config = optarg;
            break;
        default:
            if (opt == ':')
                bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            else
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    bool load = !show && !alias && !remove;
    if ((load || remove) && (alternate_root || release != NULL)) {
        bx_diag(&diag, "%s is only supported for the running kernel's installed module tree",
                remove ? "removal" : "loading");
        return 1;
    }
    if ((show && alias) || (remove && (show || alias)) || (!load && config != NULL) || optind == argc ||
        (!load && argc - optind != 1) ||
        !bx_module_alias_query_valid(argv[optind])) {
        usage(stderr, diag.progname);
        return 1;
    }
    const char* name = argv[optind];
    if (remove && !bx_module_remove_name_valid((struct bx_module_path) {name, strlen(name)})) {
        bx_diag(&diag, "invalid module name: '%s'", name);
        return 1;
    }
    struct bx_module_tree tree = {.fd = -1};
    struct bx_module_policy policy = {0};
    int status = 1;
    if (load && bx_module_policy_open(&policy, config, &diag) < 0)
        goto out;
    if (bx_module_tree_open(&tree, base, release) < 0) {
        bx_perror_path(&diag, tree.path ? tree.path : "module tree");
        goto out;
    }
    if (alias) {
        status = show_aliases(&tree, name, &diag);
        goto out;
    }
    struct bx_module_plan plan;
    int planned = remove ? bx_module_tree_remove_plan(&tree, name, &plan)
                         : bx_module_tree_plan(&tree, name, &plan);
    if (planned < 0) {
        bx_diag(&diag, "cannot plan dependencies for '%s': %s", name,
                planned == BX_MODULE_PLAN_CYCLE ? "dependency cycle" : strerror(errno));
        goto out;
    }
    if (remove) {
        status = remove_plan(&plan, &diag);
        free(plan.steps);
        goto out;
    }
    if (load) {
        char* parameters = bx_module_parameters(argc - optind - 1, argv + optind + 1);
        if (parameters == NULL)
            bx_perror_path(&diag, "module parameters");
        else
            status = load_plan(&tree, &plan, &policy, parameters, &diag);
        free(parameters);
        free(plan.steps);
        goto out;
    }
    bool printed = true;
    for (size_t i = 0; printed && i < plan.count; i++) {
        struct bx_module_step step = plan.steps[i];
        struct bx_module_path target = step.target;
        printed = step.builtin ? print_name("builtin ", target.text, target.len)
                               : printf("insmod %s/%.*s\n", tree.path, (int)target.len, target.text) >= 0;
    }
    free(plan.steps);
    if (!printed || fflush(stdout) == EOF)
        bx_perror_path(&diag, "standard output");
    else
        status = 0;
out:
    bx_module_policy_close(&policy);
    bx_module_tree_close(&tree);
    return status;
#else
    bx_diag(&diag, "Linux module dependency queries are not supported on this platform");
    return 1;
#endif
}
