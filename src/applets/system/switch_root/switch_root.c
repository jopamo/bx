#define _GNU_SOURCE

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

struct bx_switch_root_options {
    const char* progname;
    bool show_help;
    bool show_version;
    const char* new_root;
    int init_index;
};

struct bx_switch_root_pseudo_mount {
    const char* source_path;
    const char* target_relpath;
};

static const struct bx_switch_root_pseudo_mount bx_switch_root_pseudo_mounts[] = {
    {"/proc", "proc"},
    {"/sys", "sys"},
    {"/dev", "dev"},
    {"/run", "run"},
};

static void bx_switch_root_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... NEW_ROOT INIT [ARG]...\n", progname);
    fprintf(stream, "Move into NEW_ROOT and exec INIT directly.\n");
    fprintf(stream, "\n");
    fprintf(stream, "Intended for initramfs handoff as PID 1.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static bool bx_switch_root_parse_options(int argc, char** argv, struct bx_switch_root_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "switch_root");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hV", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    int remaining = argc - optind;
    if (remaining <= 0) {
        bx_diag(diag, "missing operands: NEW_ROOT and INIT are required");
        return false;
    }
    if (remaining == 1) {
        bx_diag(diag, "missing operand: INIT");
        return false;
    }

    options->new_root = argv[optind];
    options->init_index = optind + 1;
    return true;
}

static bool bx_switch_root_is_root_path(const char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    return len == 1 && path[0] == '/';
}

static char* bx_switch_root_join_paths(const char* left, const char* right) {
    size_t left_len = strlen(left);
    while (left_len > 1 && left[left_len - 1] == '/') {
        left_len--;
    }

    size_t right_start = 0;
    while (right[right_start] == '/') {
        right_start++;
    }

    size_t right_len = strlen(right + right_start);
    bool need_sep = (left_len > 0 && !(left_len == 1 && left[0] == '/'));

    size_t out_len = left_len + (need_sep ? 1 : 0) + right_len;
    char* out = xmalloc(out_len + 1);

    size_t pos = 0;
    if (left_len > 0) {
        memcpy(out + pos, left, left_len);
        pos += left_len;
    }
    if (need_sep) {
        out[pos++] = '/';
    }
    if (right_len > 0) {
        memcpy(out + pos, right + right_start, right_len);
        pos += right_len;
    }
    out[pos] = '\0';

    return out;
}

static bool bx_switch_root_path_is_mountpoint(const char* path, bool* is_mountpoint_out, struct bx_diag_ctx* diag, const char* label) {
    struct stat st;
    if (stat(path, &st) != 0) {
        bx_diag(diag, "cannot stat %s '%s': %s", label, path, strerror(errno));
        return false;
    }

    size_t path_len = strlen(path);
    char* parent = xmalloc(path_len + 4);
    memcpy(parent, path, path_len);
    memcpy(parent + path_len, "/..", 4);

    struct stat parent_st;
    if (stat(parent, &parent_st) != 0) {
        bx_diag(diag, "cannot stat parent of %s '%s': %s", label, path, strerror(errno));
        free(parent);
        return false;
    }

    free(parent);

    *is_mountpoint_out = (st.st_dev != parent_st.st_dev) || (st.st_ino == parent_st.st_ino);
    return true;
}

static bool bx_switch_root_validate_new_root(const char* new_root, struct bx_diag_ctx* diag) {
    if (new_root == NULL || new_root[0] == '\0') {
        bx_diag(diag, "NEW_ROOT is empty");
        return false;
    }

    if (new_root[0] != '/') {
        bx_diag(diag, "NEW_ROOT must be an absolute path: '%s'", new_root);
        return false;
    }

    if (bx_switch_root_is_root_path(new_root)) {
        bx_diag(diag, "NEW_ROOT must not be '/' for initramfs handoff");
        return false;
    }

    struct stat st;
    if (stat(new_root, &st) != 0) {
        bx_diag(diag, "cannot access NEW_ROOT '%s': %s", new_root, strerror(errno));
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        bx_diag(diag, "NEW_ROOT '%s' is not a directory", new_root);
        return false;
    }

    bool is_mountpoint = false;
    if (!bx_switch_root_path_is_mountpoint(new_root, &is_mountpoint, diag, "NEW_ROOT")) {
        return false;
    }
    if (!is_mountpoint) {
        bx_diag(diag, "NEW_ROOT '%s' is not a mount point", new_root);
        return false;
    }

    return true;
}

static bool bx_switch_root_candidate_is_executable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        return false;
    }

    return access(path, X_OK) == 0;
}

static bool bx_switch_root_validate_init_path(const char* new_root, const char* init_path, struct bx_diag_ctx* diag) {
    char* candidate = bx_switch_root_join_paths(new_root, init_path);

    struct stat st;
    if (stat(candidate, &st) != 0) {
        if (errno == ENOENT) {
            bx_diag(diag, "target init path '%s' does not exist in NEW_ROOT", init_path);
        }
        else {
            bx_diag(diag, "cannot access target init path '%s' in NEW_ROOT: %s", init_path, strerror(errno));
        }
        free(candidate);
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        bx_diag(diag, "target init path '%s' is a directory in NEW_ROOT", init_path);
        free(candidate);
        return false;
    }

    if (access(candidate, X_OK) != 0) {
        bx_diag(diag, "target init path '%s' is not executable in NEW_ROOT: %s", init_path, strerror(errno));
        free(candidate);
        return false;
    }

    free(candidate);
    return true;
}

static bool bx_switch_root_validate_init_command(const char* new_root, const char* init_command, struct bx_diag_ctx* diag) {
    const char* path_env = getenv("PATH");
    if (path_env == NULL || path_env[0] == '\0') {
        path_env = "/sbin:/bin:/usr/sbin:/usr/bin";
    }

    char* path_copy = xstrdup(path_env);
    bool found = false;

    char* segment = path_copy;
    while (true) {
        char* separator = strchr(segment, ':');
        if (separator != NULL) {
            *separator = '\0';
        }

        const char* directory = (segment[0] == '\0') ? "." : segment;
        char* relative_candidate = bx_switch_root_join_paths(directory, init_command);
        char* full_candidate = bx_switch_root_join_paths(new_root, relative_candidate);

        if (bx_switch_root_candidate_is_executable(full_candidate)) {
            found = true;
            free(relative_candidate);
            free(full_candidate);
            break;
        }

        free(relative_candidate);
        free(full_candidate);

        if (separator == NULL) {
            break;
        }
        segment = separator + 1;
    }

    free(path_copy);

    if (!found) {
        bx_diag(diag, "target init command '%s' was not found in PATH within NEW_ROOT", init_command);
        return false;
    }

    return true;
}

static bool bx_switch_root_validate_init_target(const struct bx_switch_root_options* options, int argc, char** argv, struct bx_diag_ctx* diag) {
    if (options->init_index >= argc) {
        bx_diag(diag, "missing operand: INIT");
        return false;
    }

    const char* init_target = argv[options->init_index];
    if (init_target == NULL || init_target[0] == '\0') {
        bx_diag(diag, "INIT is empty");
        return false;
    }

    if (strchr(init_target, '/') != NULL) {
        if (init_target[0] != '/') {
            bx_diag(diag, "INIT path must be absolute when containing '/': '%s'", init_target);
            return false;
        }
        return bx_switch_root_validate_init_path(options->new_root, init_target, diag);
    }

    return bx_switch_root_validate_init_command(options->new_root, init_target, diag);
}

static bool bx_switch_root_move_one_pseudo_mount(const char* source_path, const char* target_relpath, const char* new_root, struct bx_diag_ctx* diag) {
    struct stat source_st;
    if (stat(source_path, &source_st) != 0) {
        if (errno == ENOENT) {
            return true;
        }
        bx_diag(diag, "cannot access pseudo-filesystem path '%s': %s", source_path, strerror(errno));
        return false;
    }

    bool source_is_mountpoint = false;
    if (!bx_switch_root_path_is_mountpoint(source_path, &source_is_mountpoint, diag, "pseudo-filesystem path")) {
        return false;
    }

    if (!source_is_mountpoint) {
        return true;
    }

    struct stat target_st;
    if (stat(target_relpath, &target_st) != 0) {
        bx_diag(diag, "NEW_ROOT '%s' is missing required directory for '%s': %s", new_root, source_path, strerror(errno));
        return false;
    }
    if (!S_ISDIR(target_st.st_mode)) {
        bx_diag(diag, "NEW_ROOT '%s' entry '%s' is not a directory", new_root, target_relpath);
        return false;
    }

    if (mount(source_path, target_relpath, NULL, MS_MOVE, NULL) != 0) {
        bx_diag(diag, "cannot move '%s' into NEW_ROOT '%s/%s': %s", source_path, new_root, target_relpath, strerror(errno));
        return false;
    }

    return true;
}

static int bx_switch_root_exec_init(const struct bx_switch_root_options* options, char** argv, struct bx_diag_ctx* diag) {
    char** init_argv = argv + options->init_index;

    if (strchr(init_argv[0], '/') != NULL) {
        execv(init_argv[0], init_argv);
    }
    else {
        execvp(init_argv[0], init_argv);
    }

    int exec_error = errno;
    bx_diag(diag, "failed to exec INIT '%s': %s", init_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_switch_root_main(int argc, char** argv) {
    struct bx_switch_root_options options;
    struct bx_diag_ctx diag = {
        .progname = "switch_root",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_switch_root_parse_options(argc, argv, &options, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_switch_root_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (getpid() != 1) {
        bx_diag(&diag, "refusing to run when not PID 1");
        return 1;
    }

    if (!bx_switch_root_validate_new_root(options.new_root, &diag)) {
        return 1;
    }

    if (!bx_switch_root_validate_init_target(&options, argc, argv, &diag)) {
        return 1;
    }

    if (chdir(options.new_root) != 0) {
        bx_diag(&diag, "cannot change directory to NEW_ROOT '%s': %s", options.new_root, strerror(errno));
        return 1;
    }

    for (size_t i = 0; i < sizeof(bx_switch_root_pseudo_mounts) / sizeof(bx_switch_root_pseudo_mounts[0]); i++) {
        const struct bx_switch_root_pseudo_mount* pseudo = &bx_switch_root_pseudo_mounts[i];
        if (!bx_switch_root_move_one_pseudo_mount(pseudo->source_path, pseudo->target_relpath, options.new_root, &diag)) {
            return 1;
        }
    }

    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) {
        bx_diag(&diag, "cannot move NEW_ROOT '%s' over '/': %s", options.new_root, strerror(errno));
        return 1;
    }

    if (chroot(".") != 0) {
        bx_diag(&diag, "cannot chroot into NEW_ROOT '%s': %s", options.new_root, strerror(errno));
        return 1;
    }

    if (chdir("/") != 0) {
        bx_diag(&diag, "cannot change working directory to '/': %s", strerror(errno));
        return 1;
    }

    return bx_switch_root_exec_init(&options, argv, &diag);
}
