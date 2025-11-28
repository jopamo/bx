#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum bx_init_mode {
    BX_INIT_MODE_EXEC = 0,
    BX_INIT_MODE_SWITCH_ROOT,
    BX_INIT_MODE_RESCUE_SHELL,
    BX_INIT_MODE_SERVICE_SUPERVISOR,
};

struct bx_init_shutdown_mount {
    char* target;
    bool lazy;
};

struct bx_init_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool mount_pseudo;
    bool mode_selected;
    enum bx_init_mode mode;
    const char* switch_root_new_root;
    const char* rescue_shell;
    const char* service_file;
    int command_index;
    struct bx_init_shutdown_mount* shutdown_mounts;
    size_t shutdown_mount_count;
};

struct bx_init_pseudo_mount {
    const char* source;
    const char* target;
    const char* fstype;
    const char* options;
};

struct bx_init_service {
    char* name;
    char** argv;
    size_t argc;
    pid_t pid;
};

struct bx_init_service_config {
    struct bx_init_service* services;
    size_t service_count;
};

struct bx_init_token_list {
    char** items;
    size_t count;
};

static volatile sig_atomic_t bx_init_shutdown_requested = 0;

static const struct bx_init_pseudo_mount bx_init_pseudo_mounts[] = {
    {"proc", "/proc", "proc", NULL},
    {"sysfs", "/sys", "sysfs", NULL},
    {"tmpfs", "/dev", "tmpfs", "mode=0755,nosuid"},
    {"tmpfs", "/run", "tmpfs", "mode=0755,nosuid,nodev"},
};

static void bx_init_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... PROGRAM [ARG]...\n", progname);
    fprintf(stream, "       %s [OPTION]... --switch-root=NEW_ROOT INIT [ARG]...\n", progname);
    fprintf(stream, "       %s [OPTION]... --rescue-shell=SHELL [ARG]...\n", progname);
    fprintf(stream, "       %s [OPTION]... --service-file=FILE\n", progname);
    fprintf(stream, "Run the selected init payload path directly.\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase stays shell-free in the boot path: it can mount pseudo-fs,\n");
    fprintf(stream, "hand off via bx switch_root, launch an explicit rescue shell, or\n");
    fprintf(stream, "supervise service entries declared as 'service NAME -- ARG...'.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help                display this help and exit\n");
    fprintf(stream, "  -m, --mount-pseudo        ensure /proc, /sys, /dev, and /run are mounted\n");
    fprintf(stream, "  -s, --switch-root=DIR     hand off via bx switch_root DIR INIT [ARG]...\n");
    fprintf(stream, "  -r, --rescue-shell=SHELL  exec SHELL explicitly for recovery work\n");
    fprintf(stream, "  -c, --service-file=FILE   supervise 'service NAME -- ARG...' entries\n");
    fprintf(stream, "      --shutdown-umount=TARGET\n");
    fprintf(stream, "                            unmount TARGET during service shutdown\n");
    fprintf(stream, "      --shutdown-umount-lazy=TARGET\n");
    fprintf(stream, "                            lazy-unmount TARGET during service shutdown\n");
    fprintf(stream, "  -V, --version             output version information and exit\n");
}

static void bx_init_cleanup_options(struct bx_init_options* options) {
    if (options == NULL) {
        return;
    }

    for (size_t i = 0; i < options->shutdown_mount_count; i++) {
        free(options->shutdown_mounts[i].target);
    }
    free(options->shutdown_mounts);
    options->shutdown_mounts = NULL;
    options->shutdown_mount_count = 0;
}

static void bx_init_cleanup_tokens(struct bx_init_token_list* tokens) {
    if (tokens == NULL) {
        return;
    }

    for (size_t i = 0; i < tokens->count; i++) {
        free(tokens->items[i]);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
}

static void bx_init_cleanup_service_config(struct bx_init_service_config* config) {
    if (config == NULL) {
        return;
    }

    for (size_t i = 0; i < config->service_count; i++) {
        struct bx_init_service* service = &config->services[i];
        free(service->name);
        for (size_t j = 0; j < service->argc; j++) {
            free(service->argv[j]);
        }
        free(service->argv);
    }
    free(config->services);
    config->services = NULL;
    config->service_count = 0;
}

static bool bx_init_select_mode(struct bx_init_options* options, enum bx_init_mode mode, const char* option_name, struct bx_diag_ctx* diag) {
    if (!options->mode_selected) {
        options->mode_selected = true;
        options->mode = mode;
        return true;
    }

    if (options->mode != mode) {
        bx_diag(diag, "option '%s' conflicts with another selected init mode", option_name);
        return false;
    }

    bx_diag(diag, "option '%s' was specified more than once", option_name);
    return false;
}

static bool bx_init_append_shutdown_mount(struct bx_init_options* options, const char* target, bool lazy, struct bx_diag_ctx* diag) {
    if (target == NULL || target[0] == '\0') {
        bx_diag(diag, "shutdown unmount target may not be empty");
        return false;
    }

    struct bx_init_shutdown_mount* resized = xrealloc(options->shutdown_mounts, (options->shutdown_mount_count + 1u) * sizeof(*options->shutdown_mounts));
    options->shutdown_mounts = resized;
    options->shutdown_mounts[options->shutdown_mount_count].target = xstrdup(target);
    options->shutdown_mounts[options->shutdown_mount_count].lazy = lazy;
    options->shutdown_mount_count++;
    return true;
}

static bool bx_init_parse_options(int argc, char** argv, struct bx_init_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"mount-pseudo", no_argument, NULL, 'm'},
        {"switch-root", required_argument, NULL, 's'},
        {"rescue-shell", required_argument, NULL, 'r'},
        {"service-file", required_argument, NULL, 'c'},
        {"shutdown-umount", required_argument, NULL, 1},
        {"shutdown-umount-lazy", required_argument, NULL, 2},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "init");
    options->mode = BX_INIT_MODE_EXEC;
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hms:r:c:V", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->show_help = true;
                return true;
            case 'm':
                options->mount_pseudo = true;
                break;
            case 's':
                if (optarg == NULL || optarg[0] == '\0') {
                    bx_diag(diag, "switch-root target may not be empty");
                    return false;
                }
                if (!bx_init_select_mode(options, BX_INIT_MODE_SWITCH_ROOT, "--switch-root", diag)) {
                    return false;
                }
                options->switch_root_new_root = optarg;
                break;
            case 'r':
                if (optarg == NULL || optarg[0] == '\0') {
                    bx_diag(diag, "rescue shell path may not be empty");
                    return false;
                }
                if (!bx_init_select_mode(options, BX_INIT_MODE_RESCUE_SHELL, "--rescue-shell", diag)) {
                    return false;
                }
                options->rescue_shell = optarg;
                break;
            case 'c':
                if (optarg == NULL || optarg[0] == '\0') {
                    bx_diag(diag, "service file path may not be empty");
                    return false;
                }
                if (!bx_init_select_mode(options, BX_INIT_MODE_SERVICE_SUPERVISOR, "--service-file", diag)) {
                    return false;
                }
                options->service_file = optarg;
                break;
            case 1:
                if (!bx_init_append_shutdown_mount(options, optarg, false, diag)) {
                    return false;
                }
                break;
            case 2:
                if (!bx_init_append_shutdown_mount(options, optarg, true, diag)) {
                    return false;
                }
                break;
            case 'V':
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    if (options->shutdown_mount_count > 0 && options->mode != BX_INIT_MODE_SERVICE_SUPERVISOR) {
        bx_diag(diag, "shutdown unmounts require --service-file mode");
        return false;
    }

    options->command_index = optind;

    switch (options->mode) {
        case BX_INIT_MODE_EXEC:
            if (optind >= argc) {
                bx_diag(diag, "missing operand: PROGRAM");
                return false;
            }
            return true;
        case BX_INIT_MODE_SWITCH_ROOT:
            if (optind >= argc) {
                bx_diag(diag, "missing operand: INIT");
                return false;
            }
            return true;
        case BX_INIT_MODE_RESCUE_SHELL:
            return true;
        case BX_INIT_MODE_SERVICE_SUPERVISOR:
            if (optind < argc) {
                bx_diag(diag, "service-file mode does not accept PROGRAM operands");
                return false;
            }
            return true;
    }

    bx_diag(diag, "internal error: unknown init mode");
    return false;
}

static int bx_init_exec_program_argv(char** command_argv, struct bx_diag_ctx* diag, const char* description) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "cannot execute %s '%s': %s", description, command_argv[0], strerror(exec_error));

    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

static int bx_init_exec_program(const struct bx_init_options* options, char** argv, struct bx_diag_ctx* diag) {
    char** command_argv = argv + options->command_index;
    return bx_init_exec_program_argv(command_argv, diag, "PROGRAM");
}

static int bx_init_exec_rescue_shell(const struct bx_init_options* options, int argc, char** argv, struct bx_diag_ctx* diag) {
    int extra_argc = argc - options->command_index;
    char** shell_argv = xmalloc(((size_t)extra_argc + 2u) * sizeof(*shell_argv));
    shell_argv[0] = xstrdup(options->rescue_shell);
    for (int i = 0; i < extra_argc; i++) {
        shell_argv[i + 1] = argv[options->command_index + i];
    }
    shell_argv[extra_argc + 1] = NULL;

    int status = bx_init_exec_program_argv(shell_argv, diag, "rescue shell");
    free(shell_argv[0]);
    free(shell_argv);
    return status;
}

static int bx_init_exec_switch_root(const struct bx_init_options* options, int argc, char** argv) {
    int init_argc = argc - options->command_index;
    int switch_argc = init_argc + 2;

    char** switch_argv = xmalloc(((size_t)switch_argc + 1u) * sizeof(*switch_argv));
    switch_argv[0] = xstrdup("switch_root");
    switch_argv[1] = xstrdup(options->switch_root_new_root);
    for (int i = 0; i < init_argc; i++) {
        switch_argv[i + 2] = argv[options->command_index + i];
    }
    switch_argv[switch_argc] = NULL;

    int status = bx_switch_root_main(switch_argc, switch_argv);
    free(switch_argv[0]);
    free(switch_argv[1]);
    free(switch_argv);
    return status;
}

static bool bx_init_path_is_mountpoint(const char* path, bool* is_mountpoint_out, struct bx_diag_ctx* diag) {
    struct stat path_stat;
    if (stat(path, &path_stat) != 0) {
        bx_diag(diag, "cannot stat '%s': %s", path, strerror(errno));
        return false;
    }

    if (strcmp(path, "/") == 0) {
        *is_mountpoint_out = true;
        return true;
    }

    size_t path_len = strlen(path);
    char* parent_path = malloc(path_len + 4u);
    if (parent_path == NULL) {
        bx_diag(diag, "memory allocation failure while checking mountpoint '%s'", path);
        return false;
    }

    memcpy(parent_path, path, path_len);
    memcpy(parent_path + path_len, "/..", 4u);

    struct stat parent_stat;
    if (stat(parent_path, &parent_stat) != 0) {
        int parent_stat_error = errno;
        bx_diag(diag, "cannot stat '%s': %s", parent_path, strerror(parent_stat_error));
        free(parent_path);
        return false;
    }
    free(parent_path);

    *is_mountpoint_out = (path_stat.st_dev != parent_stat.st_dev) || (path_stat.st_ino == parent_stat.st_ino);
    return true;
}

static bool bx_init_ensure_mount_dir(const char* path, struct bx_diag_ctx* diag) {
    if (mkdir(path, 0755) == 0) {
        return true;
    }

    if (errno != EEXIST) {
        bx_diag(diag, "cannot create pseudo-fs mountpoint '%s': %s", path, strerror(errno));
        return false;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        bx_diag(diag, "cannot stat pseudo-fs mountpoint '%s': %s", path, strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        bx_diag(diag, "pseudo-fs mountpoint '%s' exists but is not a directory", path);
        return false;
    }
    return true;
}

static int bx_init_mount_one_pseudo(const struct bx_init_pseudo_mount* pseudo, struct bx_diag_ctx* diag) {
    char* fstype_copy = strdup(pseudo->fstype);
    char* source_copy = strdup(pseudo->source);
    char* target_copy = strdup(pseudo->target);
    char* options_copy = NULL;
    if (pseudo->options != NULL && pseudo->options[0] != '\0') {
        options_copy = strdup(pseudo->options);
    }

    if (fstype_copy == NULL || source_copy == NULL || target_copy == NULL || ((pseudo->options != NULL && pseudo->options[0] != '\0') && options_copy == NULL)) {
        bx_diag(diag, "memory allocation failure while preparing pseudo-fs mount '%s' -> '%s'", pseudo->source, pseudo->target);
        free(fstype_copy);
        free(source_copy);
        free(target_copy);
        free(options_copy);
        return 1;
    }

    char* mount_argv[8];
    int mount_argc = 0;

    mount_argv[mount_argc++] = "mount";
    mount_argv[mount_argc++] = "-t";
    mount_argv[mount_argc++] = fstype_copy;
    if (options_copy != NULL && options_copy[0] != '\0') {
        mount_argv[mount_argc++] = "-o";
        mount_argv[mount_argc++] = options_copy;
    }
    mount_argv[mount_argc++] = source_copy;
    mount_argv[mount_argc++] = target_copy;
    mount_argv[mount_argc] = NULL;

    int mount_status = bx_mount_main(mount_argc, mount_argv);
    if (mount_status != 0) {
        bx_diag(diag, "failed to mount pseudo-fs '%s' on '%s' via bx mount", pseudo->fstype, pseudo->target);
    }

    free(fstype_copy);
    free(source_copy);
    free(target_copy);
    free(options_copy);
    return mount_status;
}

static int bx_init_mount_pseudo_filesystems(struct bx_diag_ctx* diag) {
    for (size_t i = 0; i < sizeof(bx_init_pseudo_mounts) / sizeof(bx_init_pseudo_mounts[0]); i++) {
        const struct bx_init_pseudo_mount* pseudo = &bx_init_pseudo_mounts[i];

        if (!bx_init_ensure_mount_dir(pseudo->target, diag)) {
            return 1;
        }

        bool is_mountpoint = false;
        if (!bx_init_path_is_mountpoint(pseudo->target, &is_mountpoint, diag)) {
            return 1;
        }
        if (is_mountpoint) {
            continue;
        }

        int mount_status = bx_init_mount_one_pseudo(pseudo, diag);
        if (mount_status != 0) {
            return mount_status;
        }
    }

    return 0;
}

static bool bx_init_tokenize_line(const char* line, struct bx_init_token_list* tokens) {
    memset(tokens, 0, sizeof(*tokens));

    const char* cursor = line;
    while (*cursor != '\0') {
        while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
            cursor++;
        }
        if (*cursor == '\0' || *cursor == '#') {
            break;
        }

        const char* start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }

        size_t token_len = (size_t)(cursor - start);
        char* token = xmalloc(token_len + 1u);
        memcpy(token, start, token_len);
        token[token_len] = '\0';

        char** resized = xrealloc(tokens->items, (tokens->count + 1u) * sizeof(*tokens->items));
        tokens->items = resized;
        tokens->items[tokens->count++] = token;
    }

    return true;
}

static bool bx_init_add_service(struct bx_init_service_config* config, const struct bx_init_token_list* tokens, const char* path, size_t line_number, struct bx_diag_ctx* diag) {
    if (tokens->count < 4 || strcmp(tokens->items[2], "--") != 0) {
        bx_diag(diag, "service file '%s' line %zu must be 'service NAME -- ARG...'", path, line_number);
        return false;
    }

    struct bx_init_service* resized = xrealloc(config->services, (config->service_count + 1u) * sizeof(*config->services));
    config->services = resized;

    struct bx_init_service* service = &config->services[config->service_count];
    memset(service, 0, sizeof(*service));
    service->name = xstrdup(tokens->items[1]);
    service->argc = tokens->count - 3u;
    service->argv = xmalloc((service->argc + 1u) * sizeof(*service->argv));
    for (size_t i = 0; i < service->argc; i++) {
        service->argv[i] = xstrdup(tokens->items[i + 3u]);
    }
    service->argv[service->argc] = NULL;
    service->pid = 0;

    config->service_count++;
    return true;
}

static bool bx_init_parse_service_file(const char* path, struct bx_init_service_config* config, struct bx_diag_ctx* diag) {
    memset(config, 0, sizeof(*config));

    FILE* stream = fopen(path, "r");
    if (stream == NULL) {
        bx_diag(diag, "cannot open service file '%s': %s", path, strerror(errno));
        return false;
    }

    bool ok = true;
    char* line = NULL;
    size_t line_cap = 0;
    size_t line_number = 0;

    while (true) {
        ssize_t line_len = getline(&line, &line_cap, stream);
        if (line_len < 0) {
            break;
        }

        line_number++;
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }

        struct bx_init_token_list tokens;
        bx_init_tokenize_line(line, &tokens);
        if (tokens.count == 0) {
            bx_init_cleanup_tokens(&tokens);
            continue;
        }

        if (strcmp(tokens.items[0], "service") == 0) {
            ok = bx_init_add_service(config, &tokens, path, line_number, diag);
        }
        else {
            bx_diag(diag, "service file '%s' line %zu has unknown directive '%s'", path, line_number, tokens.items[0]);
            ok = false;
        }

        bx_init_cleanup_tokens(&tokens);
        if (!ok) {
            break;
        }
    }

    if (ok && ferror(stream)) {
        bx_diag(diag, "cannot read service file '%s': %s", path, strerror(errno));
        ok = false;
    }

    free(line);
    fclose(stream);

    if (ok && config->service_count == 0) {
        bx_diag(diag, "service file '%s' does not declare any services", path);
        ok = false;
    }

    if (!ok) {
        bx_init_cleanup_service_config(config);
    }

    return ok;
}

static void bx_init_handle_shutdown_signal(int signo) {
    (void)signo;
    bx_init_shutdown_requested = 1;
}

static bool bx_init_install_signal_handlers(struct bx_diag_ctx* diag) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = bx_init_handle_shutdown_signal;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        bx_diag(diag, "cannot install SIGTERM handler: %s", strerror(errno));
        return false;
    }
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        bx_diag(diag, "cannot install SIGINT handler: %s", strerror(errno));
        return false;
    }
    if (sigaction(SIGHUP, &sa, NULL) != 0) {
        bx_diag(diag, "cannot install SIGHUP handler: %s", strerror(errno));
        return false;
    }

    signal(SIGPIPE, SIG_IGN);
    return true;
}

static void bx_init_reset_child_signals(void) {
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

static struct bx_init_service* bx_init_find_service_by_pid(struct bx_init_service_config* config, pid_t pid) {
    for (size_t i = 0; i < config->service_count; i++) {
        if (config->services[i].pid == pid) {
            return &config->services[i];
        }
    }

    return NULL;
}

static bool bx_init_spawn_service(struct bx_init_service* service, struct bx_diag_ctx* diag) {
    pid_t pid = fork();
    if (pid < 0) {
        bx_diag(diag, "cannot fork service '%s': %s", service->name, strerror(errno));
        return false;
    }

    if (pid == 0) {
        bx_init_reset_child_signals();
        if (setpgid(0, 0) != 0) {
            fprintf(stderr, "%s: cannot place service '%s' in its own process group: %s\n", diag->progname, service->name, strerror(errno));
            _exit(1);
        }

        execvp(service->argv[0], service->argv);
        fprintf(stderr, "%s: cannot execute service '%s' command '%s': %s\n", diag->progname, service->name, service->argv[0], strerror(errno));
        _exit((errno == ENOENT) ? 127 : 126);
    }

    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
        bx_diag(diag, "cannot place service '%s' in its own process group: %s", service->name, strerror(errno));
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return false;
    }

    service->pid = pid;
    return true;
}

static bool bx_init_any_service_running(const struct bx_init_service_config* config) {
    for (size_t i = 0; i < config->service_count; i++) {
        if (config->services[i].pid > 0) {
            return true;
        }
    }

    return false;
}

static void bx_init_clear_all_service_pids(struct bx_init_service_config* config) {
    for (size_t i = 0; i < config->service_count; i++) {
        config->services[i].pid = 0;
    }
}

static void bx_init_signal_service_group(const struct bx_init_service* service, int signo, struct bx_diag_ctx* diag) {
    if (service->pid <= 0) {
        return;
    }

    if (kill(-service->pid, signo) == 0 || errno == ESRCH) {
        return;
    }

    if (kill(service->pid, signo) == 0 || errno == ESRCH) {
        return;
    }

    bx_diag(diag, "cannot signal service '%s': %s", service->name, strerror(errno));
}

static void bx_init_signal_all_services(const struct bx_init_service_config* config, int signo, struct bx_diag_ctx* diag) {
    for (size_t i = 0; i < config->service_count; i++) {
        bx_init_signal_service_group(&config->services[i], signo, diag);
    }
}

static void bx_init_reap_children(struct bx_init_service_config* config, bool respawn, struct bx_diag_ctx* diag, bool* ok_out) {
    while (true) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid == 0) {
            return;
        }
        if (pid < 0) {
            if (errno == ECHILD || errno == EINTR) {
                return;
            }
            bx_diag(diag, "waitpid failed: %s", strerror(errno));
            *ok_out = false;
            return;
        }

        struct bx_init_service* service = bx_init_find_service_by_pid(config, pid);
        if (service == NULL) {
            continue;
        }

        service->pid = 0;
        if (respawn) {
            if (!bx_init_spawn_service(service, diag)) {
                *ok_out = false;
                return;
            }
        }
    }
}

static bool bx_init_shutdown_services(struct bx_init_service_config* config, struct bx_diag_ctx* diag) {
    if (!bx_init_any_service_running(config)) {
        return true;
    }

    time_t deadline = time(NULL) + 1;
    bool sent_sigkill = false;

    bx_init_signal_all_services(config, SIGTERM, diag);

    while (bx_init_any_service_running(config)) {
        bool ok = true;
        bx_init_reap_children(config, false, diag, &ok);
        if (!ok) {
            return false;
        }
        if (!bx_init_any_service_running(config)) {
            return true;
        }

        if (!sent_sigkill && time(NULL) >= deadline) {
            bx_init_signal_all_services(config, SIGKILL, diag);
            sent_sigkill = true;
        }

        usleep(100000);
    }

    if (waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD) {
        bx_init_clear_all_service_pids(config);
    }

    return true;
}

static int bx_init_shutdown_mount_compare(const void* lhs, const void* rhs) {
    const struct bx_init_shutdown_mount* left = lhs;
    const struct bx_init_shutdown_mount* right = rhs;

    size_t left_len = strlen(left->target);
    size_t right_len = strlen(right->target);
    if (left_len < right_len) {
        return 1;
    }
    if (left_len > right_len) {
        return -1;
    }

    return strcmp(left->target, right->target);
}

static int bx_init_run_shutdown_unmounts(const struct bx_init_options* options) {
    if (options->shutdown_mount_count == 0) {
        return 0;
    }

    struct bx_init_shutdown_mount* ordered = xmalloc(options->shutdown_mount_count * sizeof(*ordered));
    for (size_t i = 0; i < options->shutdown_mount_count; i++) {
        ordered[i] = options->shutdown_mounts[i];
    }

    qsort(ordered, options->shutdown_mount_count, sizeof(*ordered), bx_init_shutdown_mount_compare);

    int status = 0;
    for (size_t i = 0; i < options->shutdown_mount_count; i++) {
        char* umount_argv[4];
        int umount_argc = 0;

        umount_argv[umount_argc++] = "umount";
        if (ordered[i].lazy) {
            umount_argv[umount_argc++] = "-l";
        }
        umount_argv[umount_argc++] = ordered[i].target;
        umount_argv[umount_argc] = NULL;

        status = bx_umount_main(umount_argc, umount_argv);
        if (status != 0) {
            break;
        }
    }

    free(ordered);
    return status;
}

static int bx_init_run_service_supervisor(const struct bx_init_options* options, struct bx_diag_ctx* diag) {
    struct bx_init_service_config config;
    if (!bx_init_parse_service_file(options->service_file, &config, diag)) {
        return 1;
    }

    if (!bx_init_install_signal_handlers(diag)) {
        bx_init_cleanup_service_config(&config);
        return 1;
    }

    bx_init_shutdown_requested = 0;

    for (size_t i = 0; i < config.service_count; i++) {
        if (!bx_init_spawn_service(&config.services[i], diag)) {
            bx_init_shutdown_services(&config, diag);
            bx_init_cleanup_service_config(&config);
            return 1;
        }
    }

    while (!bx_init_shutdown_requested) {
        bool ok = true;
        bx_init_reap_children(&config, true, diag, &ok);
        if (!ok) {
            bx_init_shutdown_services(&config, diag);
            bx_init_cleanup_service_config(&config);
            return 1;
        }
        usleep(100000);
    }

    if (!bx_init_shutdown_services(&config, diag)) {
        bx_init_cleanup_service_config(&config);
        return 1;
    }

    int status = bx_init_run_shutdown_unmounts(options);
    bx_init_cleanup_service_config(&config);
    return status;
}

int bx_init_main(int argc, char** argv) {
    struct bx_init_options options;
    struct bx_diag_ctx diag = {
        .progname = "init",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_init_parse_options(argc, argv, &options, &diag)) {
        bx_cli_print_try_help(options.progname);
        bx_init_cleanup_options(&options);
        return 2;
    }

    if (options.show_help) {
        bx_init_print_help(stdout, options.progname);
        bx_init_cleanup_options(&options);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_init_cleanup_options(&options);
        return 0;
    }

    if (options.mount_pseudo) {
        int mount_status = bx_init_mount_pseudo_filesystems(&diag);
        if (mount_status != 0) {
            bx_init_cleanup_options(&options);
            return mount_status;
        }
    }

    int status = 0;
    switch (options.mode) {
        case BX_INIT_MODE_EXEC:
            status = bx_init_exec_program(&options, argv, &diag);
            break;
        case BX_INIT_MODE_SWITCH_ROOT:
            status = bx_init_exec_switch_root(&options, argc, argv);
            break;
        case BX_INIT_MODE_RESCUE_SHELL:
            status = bx_init_exec_rescue_shell(&options, argc, argv, &diag);
            break;
        case BX_INIT_MODE_SERVICE_SUPERVISOR:
            status = bx_init_run_service_supervisor(&options, &diag);
            break;
    }

    bx_init_cleanup_options(&options);
    return status;
}
