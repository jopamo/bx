#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "bx/diag.h"
#include "bx/applet_metadata.h"
#include "bx/applet_profile.h"
#include "bx/libbx.h"
#include "bx/runtime_snapshot.h"
#include "bx/self_exec.h"
#include "dispatch/dispatch.h"
#include "lib/output_policy.h"
#include "lib/path_ops.h"
#include "lib/status.h"

static const char shebang_applet_prefix[] = "--bx-applet-shebang=";
static const char profile_option_prefix[] = "--profile=";
static const char color_option_prefix[] = "--color=";
static const char config_option_prefix[] = "--config=";
static const char sandbox_option_prefix[] = "--sandbox=";

struct bx_global_options {
    const struct bx_applet_profile* profile;
    const char* profile_source;
    bool json;
    bool color_set;
    const char* color_value;
    bool no_color;
    bool trace;
    bool sandbox;
    const char* sandbox_value;
    bool config;
    const char* config_path;
    bool no_config;
};

static const char* get_shebang_applet(const char* arg) {
    size_t prefix_len = sizeof(shebang_applet_prefix) - 1;

    if (!arg || strncmp(arg, shebang_applet_prefix, prefix_len) != 0) {
        return NULL;
    }

    const char* name = arg + prefix_len;
    return (name[0] != '\0') ? name : NULL;
}

static int select_profile_name(
    const char* source,
    const char* name,
    const struct bx_applet_profile** profile,
    struct bx_diag_ctx* diag
) {
    const struct bx_applet_profile* selected = bx_applet_profile_find(name);
    if (selected == NULL) {
        bx_diag(diag, "unknown profile '%s' from %s", name != NULL ? name : "", source);
        return bx_status_error();
    }
    *profile = selected;
    return bx_status_success();
}

static int select_env_profile(
    const struct bx_applet_profile** profile,
    const char** profile_source,
    struct bx_diag_ctx* diag
) {
    const char* env_profile = getenv("BX_PROFILE");
    if (env_profile == NULL) {
        *profile = bx_applet_profile_default();
        *profile_source = "default";
        return bx_status_success();
    }
    int rc = select_profile_name("BX_PROFILE", env_profile, profile, diag);
    if (rc == 0) {
        *profile_source = "BX_PROFILE";
    }
    return rc;
}

static int ensure_selected_profile(
    struct bx_global_options* global_options,
    struct bx_diag_ctx* diag
) {
    if (global_options->profile != NULL) {
        return bx_status_success();
    }
    return select_env_profile(&global_options->profile, &global_options->profile_source, diag);
}

static int parse_global_options(
    int argc,
    char** argv,
    int* arg_index,
    struct bx_global_options* global_options,
    struct bx_diag_ctx* diag
) {
    while (*arg_index < argc) {
        const char* arg = argv[*arg_index];
        size_t profile_prefix_len = sizeof(profile_option_prefix) - 1;
        size_t color_prefix_len = sizeof(color_option_prefix) - 1;
        size_t config_prefix_len = sizeof(config_option_prefix) - 1;
        size_t sandbox_prefix_len = sizeof(sandbox_option_prefix) - 1;

        if (strcmp(arg, "--profile") == 0) {
            if (*arg_index + 1 >= argc) {
                bx_diag(diag, "missing argument for --profile");
                return bx_status_error();
            }
            if (select_profile_name("--profile", argv[*arg_index + 1], &global_options->profile, diag) != 0) {
                return bx_status_error();
            }
            global_options->profile_source = "--profile";
            *arg_index += 2;
            continue;
        }
        if (strncmp(arg, profile_option_prefix, profile_prefix_len) == 0) {
            if (select_profile_name("--profile", arg + profile_prefix_len, &global_options->profile, diag) != 0) {
                return bx_status_error();
            }
            global_options->profile_source = "--profile";
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--json") == 0) {
            global_options->json = true;
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--color") == 0 || strncmp(arg, color_option_prefix, color_prefix_len) == 0) {
            global_options->color_set = true;
            global_options->color_value = strncmp(arg, color_option_prefix, color_prefix_len) == 0
                ? arg + color_prefix_len
                : "auto";
            global_options->no_color = false;
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--no-color") == 0) {
            global_options->no_color = true;
            global_options->color_set = false;
            global_options->color_value = NULL;
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--trace") == 0) {
            global_options->trace = true;
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--sandbox") == 0 || strncmp(arg, sandbox_option_prefix, sandbox_prefix_len) == 0) {
            global_options->sandbox = true;
            global_options->sandbox_value = strncmp(arg, sandbox_option_prefix, sandbox_prefix_len) == 0
                ? arg + sandbox_prefix_len
                : "default";
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--config") == 0) {
            if (*arg_index + 1 >= argc) {
                bx_diag(diag, "missing argument for --config");
                return bx_status_error();
            }
            global_options->config = true;
            global_options->config_path = argv[*arg_index + 1];
            *arg_index += 2;
            continue;
        }
        if (strncmp(arg, config_option_prefix, config_prefix_len) == 0) {
            global_options->config = true;
            global_options->config_path = arg + config_prefix_len;
            (*arg_index)++;
            continue;
        }
        if (strcmp(arg, "--no-config") == 0) {
            global_options->no_config = true;
            (*arg_index)++;
            continue;
        }
        break;
    }

    return bx_status_success();
}

static struct bx_runtime_snapshot_spec make_runtime_snapshot_spec(
    const struct bx_global_options* global_options
) {
    enum bx_runtime_config_policy config_policy = BX_RUNTIME_CONFIG_DEFAULT;
    enum bx_runtime_color_policy color_policy = BX_RUNTIME_COLOR_AUTO;
    enum bx_runtime_terminal_policy terminal_policy = BX_RUNTIME_TERMINAL_DEFAULT;
    enum bx_runtime_sandbox_policy sandbox_policy = BX_RUNTIME_SANDBOX_DEFAULT;
    enum bx_runtime_environment_policy environment_policy = BX_RUNTIME_ENV_DEFAULT;

    if (global_options != NULL && global_options->config) {
        config_policy = BX_RUNTIME_CONFIG_EXPLICIT;
    }
    else if (global_options != NULL && global_options->no_config) {
        config_policy = BX_RUNTIME_CONFIG_DISABLED;
    }

    if (global_options != NULL && global_options->no_color) {
        color_policy = BX_RUNTIME_COLOR_DISABLED;
        terminal_policy = BX_RUNTIME_TERMINAL_COLOR_DISABLED;
    }
    else if (global_options != NULL && global_options->color_set) {
        color_policy = BX_RUNTIME_COLOR_EXPLICIT;
    }

    if (global_options != NULL && global_options->sandbox) {
        sandbox_policy = BX_RUNTIME_SANDBOX_EXPLICIT;
    }

    if (global_options != NULL && global_options->profile_source != NULL &&
        strcmp(global_options->profile_source, "BX_PROFILE") == 0) {
        environment_policy = BX_RUNTIME_ENV_PROFILE;
    }

    return (struct bx_runtime_snapshot_spec){
        .profile = global_options != NULL ? global_options->profile : NULL,
        .profile_source = global_options != NULL ? global_options->profile_source : NULL,
        .json_requested = global_options != NULL && global_options->json,
        .trace_requested = global_options != NULL && global_options->trace,
        .config_policy = config_policy,
        .config_path = global_options != NULL ? global_options->config_path : NULL,
        .color_policy = color_policy,
        .color_value = global_options != NULL ? global_options->color_value : NULL,
        .terminal_policy = terminal_policy,
        .sandbox_policy = sandbox_policy,
        .sandbox_value = global_options != NULL ? global_options->sandbox_value : NULL,
        .environment_policy = environment_policy,
    };
}

static struct bx_runtime_snapshot* create_runtime_snapshot(
    const struct bx_global_options* global_options
) {
    struct bx_runtime_snapshot_spec spec = make_runtime_snapshot_spec(global_options);
    return bx_runtime_snapshot_create(&spec);
}

static void output_policy_from_runtime(
    const struct bx_runtime_snapshot* runtime,
    struct bx_output_policy* output_policy
) {
    bx_output_policy_init_stdout(output_policy);
    bx_output_policy_set_json(output_policy, bx_runtime_snapshot_json_requested(runtime));

    switch (bx_runtime_snapshot_color_policy(runtime)) {
        case BX_RUNTIME_COLOR_EXPLICIT:
            bx_output_policy_set_color_mode(
                output_policy,
                bx_color_parse(bx_runtime_snapshot_color_value(runtime))
            );
            break;
        case BX_RUNTIME_COLOR_DISABLED:
            bx_output_policy_set_color_mode(output_policy, BX_COLOR_NEVER);
            break;
        case BX_RUNTIME_COLOR_AUTO:
        default:
            bx_output_policy_set_color_mode(output_policy, BX_COLOR_AUTO);
            break;
    }
}

static int reject_unsupported_global_options_for_applet(
    const struct bx_runtime_snapshot* runtime,
    struct bx_diag_ctx* diag
) {
    struct bx_output_policy output_policy;

    output_policy_from_runtime(runtime, &output_policy);
    if (bx_runtime_snapshot_json_requested(runtime)
        && bx_output_policy_json_requested(&output_policy)) {
        bx_diag(diag, "global option '--json' is recognized but applet JSON framing is not implemented");
        return bx_status_error();
    }
    if (bx_runtime_snapshot_color_policy(runtime) == BX_RUNTIME_COLOR_EXPLICIT) {
        bx_diag(diag, "global option '--color' is recognized but global color policy is not implemented");
        return bx_status_error();
    }
    if (bx_runtime_snapshot_color_policy(runtime) == BX_RUNTIME_COLOR_DISABLED) {
        (void)bx_output_policy_color_enabled(&output_policy);
        bx_diag(diag, "global option '--no-color' is recognized but global color policy is not implemented");
        return bx_status_error();
    }
    if (bx_runtime_snapshot_trace_requested(runtime)) {
        bx_diag(diag, "global option '--trace' is recognized but tracing is not implemented");
        return bx_status_error();
    }
    if (bx_runtime_snapshot_sandbox_policy(runtime) == BX_RUNTIME_SANDBOX_EXPLICIT) {
        bx_diag(diag, "global option '--sandbox' is recognized but sandbox policy is not implemented");
        return bx_status_error();
    }
    if (bx_runtime_snapshot_config_policy(runtime) == BX_RUNTIME_CONFIG_EXPLICIT) {
        bx_diag(diag, "global option '--config' is recognized but config loading is not implemented");
        return bx_status_error();
    }
    return bx_status_success();
}

static int check_applet_profile(
    const struct bx_applet* applet,
    const struct bx_applet_profile* profile,
    struct bx_diag_ctx* diag
) {
    char denied_names[256];
    uint32_t denied = bx_applet_profile_denied_capabilities(profile, applet);

    if (denied == 0) {
        return bx_status_success();
    }

    bx_applet_capabilities_format(denied, denied_names, sizeof(denied_names));
    bx_diag(
        diag,
        "profile '%s' denies applet '%s' capabilities: %s",
        profile != NULL ? profile->name : "",
        applet != NULL ? applet->name : "",
        denied_names
    );
    return bx_status_error();
}

static void prepare_applet_self_exec(
    const struct bx_applet* applet,
    const char* argv0
) {
    if (bx_applet_self_dispatch_required(applet)) {
        (void)bx_self_exec_initialize(argv0);
    }
    else {
        bx_self_exec_discard_handoff();
    }
}

static int run_checked_applet(
    const struct bx_applet* applet,
    const struct bx_applet_profile* profile,
    int argc,
    char** argv,
    struct bx_diag_ctx* diag
) {
    int profile_status = check_applet_profile(applet, profile, diag);
    if (profile_status != 0) {
        return profile_status;
    }
    prepare_applet_self_exec(applet, argv[0]);
    return bx_status_run_applet(applet->main, argc, argv);
}

static int run_shebang_applet(
    const struct bx_applet* applet,
    const struct bx_applet_profile* profile,
    int argc,
    char** argv,
    struct bx_diag_ctx* diag
) {
    int profile_status = check_applet_profile(applet, profile, diag);
    if (profile_status != 0) {
        return profile_status;
    }
    prepare_applet_self_exec(applet, argv[0]);

    int applet_argc = argc - 2;
    char** applet_argv = xmalloc(((size_t)applet_argc + 1) * sizeof(*applet_argv));
    const char* wrapper_name = bx_path_basename_ptr(argv[2]);
    const char* selected_name = applet->name;
    /*
     * The shebang selector is authoritative even if a wrapper is renamed.
     * Preserve only the conventional -name spelling used to request login
     * mode; arbitrary wrapper names must not silently select another policy.
     */
    if (wrapper_name[0] == '-' &&
        strcmp(wrapper_name + 1, applet->name) == 0) {
        selected_name = wrapper_name;
    }
    char* applet_argv0 = xstrdup(selected_name);

    applet_argv[0] = applet_argv0;
    for (int i = 1; i < applet_argc; i++) {
        applet_argv[i] = argv[i + 2];
    }
    applet_argv[applet_argc] = NULL;

    int rc = bx_status_run_applet(applet->main, applet_argc, applet_argv);
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
    struct bx_runtime_snapshot* runtime = create_runtime_snapshot(NULL);
    for (size_t i = 0; i < bx_runtime_snapshot_applet_count(runtime); i++) {
        const struct bx_applet* applet = bx_runtime_snapshot_applet_at(runtime, i);
        if (applet == NULL) {
            continue;
        }
        if (!applet_shortcut_install_supported(applet->name)) {
            continue;
        }
        if (install_one_applet_shortcut(bx_path, install_dir, applet->name, symlink_mode, diag) != 0) {
            status = bx_status_error();
        }
    }
    bx_runtime_snapshot_destroy(runtime);

    return status;
}

static int run_install_mode(int argc, char** argv, int first_arg_index, struct bx_diag_ctx* diag) {
    bool symlink_mode = false;
    const char* install_dir = ".";
    bool install_dir_set = false;

    for (int i = first_arg_index; i < argc; i++) {
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

    char* bx_path = NULL;
    if (bx_self_exec_initialize(argv[0])) {
        bx_path = bx_self_exec_path_dup();
    }
    if (bx_path == NULL) {
        bx_diag(
            diag,
            "cannot identify bx executable: %s",
            strerror(errno != 0 ? errno : ENOENT)
        );
        return bx_status_error();
    }
    int rc = install_missing_applets(bx_path, install_dir, symlink_mode, diag);
    free(bx_path);
    return bx_status_from_applet(rc);
}

static void print_json_string(const char* value) {
    const unsigned char* p = (const unsigned char*)(value != NULL ? value : "");

    putchar('"');
    for (; *p != '\0'; p++) {
        switch (*p) {
            case '"':
                fputs("\\\"", stdout);
                break;
            case '\\':
                fputs("\\\\", stdout);
                break;
            case '\b':
                fputs("\\b", stdout);
                break;
            case '\f':
                fputs("\\f", stdout);
                break;
            case '\n':
                fputs("\\n", stdout);
                break;
            case '\r':
                fputs("\\r", stdout);
                break;
            case '\t':
                fputs("\\t", stdout);
                break;
            default:
                if (*p < 0x20u) {
                    printf("\\u%04x", *p);
                }
                else {
                    putchar((int)*p);
                }
                break;
        }
    }
    putchar('"');
}

static void print_json_string_list(const char* const* items, size_t count) {
    putchar('[');
    for (size_t i = 0; i < count; i++) {
        if (i != 0) {
            putchar(',');
        }
        print_json_string(items[i]);
    }
    putchar(']');
}

static int run_list_applets_mode(int argc, char** argv, int first_arg_index, struct bx_diag_ctx* diag) {
    bool json = false;
    int status = bx_status_success();
    struct bx_runtime_snapshot* runtime = NULL;

    for (int i = first_arg_index; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
            continue;
        }
        bx_diag(diag, "unknown --list-applets option '%s'", argv[i]);
        return bx_status_error();
    }

    runtime = create_runtime_snapshot(NULL);
    if (!json) {
        for (size_t i = 0; i < bx_runtime_snapshot_applet_metadata_count(runtime); i++) {
            const struct bx_applet_metadata* metadata =
                bx_runtime_snapshot_applet_metadata_at(runtime, i);
            if (metadata != NULL) {
                puts(metadata->name);
            }
        }
        bx_runtime_snapshot_destroy(runtime);
        return status;
    }

    fputs("{\"manifest_version\":2,\"generated_by\":\"tools/dispatch/generate_dispatch_tables.py\",\"applets\":[", stdout);
    for (size_t i = 0; i < bx_runtime_snapshot_applet_metadata_count(runtime); i++) {
        const struct bx_applet_metadata* metadata =
            bx_runtime_snapshot_applet_metadata_at(runtime, i);

        if (metadata == NULL) {
            continue;
        }
        if (i != 0) {
            putchar(',');
        }
        fputs("{\"name\":", stdout);
        print_json_string(metadata->name);
        fputs(",\"boot_critical\":", stdout);
        fputs(metadata->boot_critical ? "true" : "false", stdout);
        fputs(",\"execution_class\":", stdout);
        print_json_string(metadata->execution_class);
        fputs(",\"capabilities\":", stdout);
        print_json_string_list(metadata->capabilities, metadata->capability_count);
        fputs(",\"aliases\":", stdout);
        print_json_string_list(metadata->aliases, metadata->alias_count);
        fputc('}', stdout);
    }
    fputs("]}\n", stdout);
    bx_runtime_snapshot_destroy(runtime);
    return status;
}

static int run_help_index_mode(int argc, char** argv, int first_arg_index, struct bx_diag_ctx* diag) {
    if (first_arg_index != argc) {
        bx_diag(diag, "unexpected --help-index operand '%s'", argv[first_arg_index]);
        return bx_status_error();
    }

    struct bx_runtime_snapshot* runtime = create_runtime_snapshot(NULL);
    for (size_t i = 0; i < bx_runtime_snapshot_applet_metadata_count(runtime); i++) {
        const struct bx_applet_metadata* metadata =
            bx_runtime_snapshot_applet_metadata_at(runtime, i);

        if (metadata == NULL) {
            continue;
        }
        printf("%s", metadata->name);
        if (metadata->alias_count != 0) {
            fputs(" (aliases:", stdout);
            for (size_t alias_index = 0; alias_index < metadata->alias_count; alias_index++) {
                printf(" %s", metadata->aliases[alias_index]);
            }
            putchar(')');
        }
        putchar('\n');
    }
    bx_runtime_snapshot_destroy(runtime);
    return bx_status_success();
}

static int run_completion_mode(int argc, char** argv, int first_arg_index, struct bx_diag_ctx* diag) {
    if (first_arg_index >= argc) {
        bx_diag(diag, "missing shell name for --completion");
        return bx_status_error();
    }
    if (first_arg_index + 1 != argc) {
        bx_diag(diag, "unexpected --completion operand '%s'", argv[first_arg_index + 1]);
        return bx_status_error();
    }
    if (strcmp(argv[first_arg_index], "bash") != 0) {
        bx_diag(diag, "unsupported completion shell '%s'", argv[first_arg_index]);
        return bx_status_error();
    }

    puts("# bx bash completion generated from the applet registry");
    puts("_bx_completion() {");
    puts("    local cur");
    puts("    cur=\"${COMP_WORDS[COMP_CWORD]}\"");
    printf("    COMPREPLY=( $(compgen -W '");
    printf("--help --version --json --color --no-color --trace --sandbox --profile --config --no-config --install --list-applets --help-index --completion");
    struct bx_runtime_snapshot* runtime = create_runtime_snapshot(NULL);
    for (size_t i = 0; i < bx_runtime_snapshot_applet_metadata_count(runtime); i++) {
        const struct bx_applet_metadata* metadata =
            bx_runtime_snapshot_applet_metadata_at(runtime, i);
        if (metadata != NULL) {
            printf(" %s", metadata->name);
        }
    }
    bx_runtime_snapshot_destroy(runtime);
    puts("' -- \"$cur\") )");
    puts("}");
    puts("complete -F _bx_completion bx");
    return bx_status_success();
}

int main(int argc, char** argv) {
    if (argc < 1)
        return bx_status_error();

    struct bx_diag_ctx diag = {
        .progname = "bx",
    };

    const char* progname = bx_path_basename_ptr(argv[0]);
    const struct bx_applet* applet = bx_applet_find(progname);
    struct bx_global_options global_options = {0};
    if (applet == NULL && progname[0] == '-' && progname[1] != '\0') {
        applet = bx_applet_find(progname + 1);
    }

    if (applet != NULL) {
        if (ensure_selected_profile(&global_options, &diag) != 0) {
            return bx_status_error();
        }
        struct bx_runtime_snapshot* runtime = create_runtime_snapshot(&global_options);
        int rc = run_checked_applet(
            applet,
            bx_runtime_snapshot_profile(runtime),
            argc,
            argv,
            &diag
        );
        bx_runtime_snapshot_destroy(runtime);
        return rc;
    }

    const char* shebang_applet = (argc >= 2) ? get_shebang_applet(argv[1]) : NULL;
    if (shebang_applet) {
        applet = bx_applet_find(shebang_applet);
        if (applet == NULL) {
            bx_diag(&diag, "unknown applet in shebang wrapper: '%s'", shebang_applet);
            return bx_status_error();
        }
        if (argc < 3) {
            bx_diag(&diag, "invalid shebang wrapper invocation");
            return bx_status_error();
        }
        if (ensure_selected_profile(&global_options, &diag) != 0) {
            return bx_status_error();
        }
        struct bx_runtime_snapshot* runtime = create_runtime_snapshot(&global_options);
        int rc = run_shebang_applet(
            applet,
            bx_runtime_snapshot_profile(runtime),
            argc,
            argv,
            &diag
        );
        bx_runtime_snapshot_destroy(runtime);
        return rc;
    }

    if (argc < 2) {
        goto usage;
    }

    int arg_index = 1;
    if (parse_global_options(argc, argv, &arg_index, &global_options, &diag) != 0) {
        return bx_status_error();
    }

    if (arg_index >= argc) {
        bx_diag(&diag, "missing subcommand after global options");
        return bx_status_error();
    }

    if (strcmp(argv[arg_index], "--help") == 0) {
        goto usage;
    }

    if (strcmp(argv[arg_index], "--install") == 0) {
        return bx_status_from_applet(run_install_mode(argc, argv, arg_index + 1, &diag));
    }

    if (strcmp(argv[arg_index], "--version") == 0) {
        printf("bx version %s\n", BX_VERSION);
        return bx_status_success();
    }

    if (strcmp(argv[arg_index], "--list-applets") == 0) {
        if (global_options.json) {
            char* list_argv[] = {argv[arg_index], "--json", NULL};
            return run_list_applets_mode(2, list_argv, 1, &diag);
        }
        return run_list_applets_mode(argc, argv, arg_index + 1, &diag);
    }

    if (strcmp(argv[arg_index], "--help-index") == 0) {
        return run_help_index_mode(argc, argv, arg_index + 1, &diag);
    }

    if (strcmp(argv[arg_index], "--completion") == 0) {
        return run_completion_mode(argc, argv, arg_index + 1, &diag);
    }

    if (argv[arg_index][0] == '-') {
        bx_diag(&diag, "unknown option '%s'", argv[arg_index]);
        return bx_status_error();
    }

    applet = bx_applet_find(argv[arg_index]);
    if (applet != NULL) {
        struct bx_runtime_snapshot* requested_runtime = create_runtime_snapshot(&global_options);
        if (reject_unsupported_global_options_for_applet(requested_runtime, &diag) != 0) {
            bx_runtime_snapshot_destroy(requested_runtime);
            return bx_status_error();
        }
        bx_runtime_snapshot_destroy(requested_runtime);
        if (ensure_selected_profile(&global_options, &diag) != 0) {
            return bx_status_error();
        }
        struct bx_runtime_snapshot* runtime = create_runtime_snapshot(&global_options);
        int rc = run_checked_applet(
            applet,
            bx_runtime_snapshot_profile(runtime),
            argc - arg_index,
            argv + arg_index,
            &diag
        );
        bx_runtime_snapshot_destroy(runtime);
        return rc;
    }

    bx_diag(&diag, "unknown subcommand '%s'", argv[arg_index]);
    return bx_status_error();

usage:
    printf("usage: bx SUBCOMMAND [options] ...\n");
    printf("       bx [--profile PROFILE] SUBCOMMAND [options] ...\n");
    printf("       bx --install [-s|--symlink] [DIR]\n");
    printf("\n");
    printf("Global profile selection also honors BX_PROFILE when dispatching applets.\n");
    printf("Recognized global options: --help, --version, --json, --color, --no-color,\n");
    printf("--trace, --sandbox, --profile, --config, and --no-config.\n");
    printf("--list-applets [--json], --help-index, and --completion bash are generated from the applet registry.\n");
    printf("\n");
    printf("--install creates shortcuts only for applets missing in DIR.\n");
    printf("Use -s to create symlinks (default is hard links).\n");
    printf("\n");
    printf("Profiles:\n");
    for (size_t i = 0; i < bx_applet_profile_count(); i++) {
        const struct bx_applet_profile* profile_entry = bx_applet_profile_at(i);
        if (profile_entry != NULL) {
            printf("  %s\n", profile_entry->name);
        }
    }
    printf("\n");
    printf("Currently supported subcommands:\n");
    struct bx_runtime_snapshot* runtime = create_runtime_snapshot(NULL);
    for (size_t i = 0; i < bx_runtime_snapshot_applet_count(runtime); i++) {
        const struct bx_applet* applet_entry = bx_runtime_snapshot_applet_at(runtime, i);
        if (applet_entry != NULL) {
            printf("  %s\n", applet_entry->name);
        }
    }
    bx_runtime_snapshot_destroy(runtime);
    return bx_status_success();
}
