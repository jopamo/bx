#define _GNU_SOURCE

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <sys/statfs.h>
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
#include "lib/child_runner.h"
#include "lib/cli_common.h"
#include "lib/path_ops.h"
#include "lib/open_resolve.h"
#include "lib/option_scan.h"
#include "lib/statx_compat.h"
#include "lib/fd_ops.h"
#include "applets/system/switch_root/switch_root.h"

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
    static const struct bx_option_spec specs[] = {
        {'h', 'h', "help", BX_OPTION_NO_VALUE},
        {'V', 'V', "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "switch_root");
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    size_t first;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END) {
            first = result.index;
            break;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.short_name)
                bx_diag(diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", result.token);
            return false;
        }

        switch (result.spec->id) {
            case 'h':
                options->show_help = true;
                return true;
            case 'V':
                options->show_version = true;
                return true;
        }
    }

    size_t remaining = (size_t)argc - first;
    if (remaining == 0) {
        bx_diag(diag, "missing operands: NEW_ROOT and INIT are required");
        return false;
    }
    if (remaining == 1) {
        bx_diag(diag, "missing operand: INIT");
        return false;
    }

    options->new_root = argv[first];
    options->init_index = (int)first + 1;
    return true;
}

static bool bx_switch_root_is_root_path(const char* path) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        len--;
    }

    return len == 1 && path[0] == '/';
}

static bool bx_switch_root_path_is_mountpoint(int dirfd, const char* path, bool* is_mountpoint_out,
                                              struct bx_diag_ctx* diag, const char* label) {
#ifdef STATX_ATTR_MOUNT_ROOT
    struct statx info;
    if (statx(dirfd, path, 0, STATX_TYPE, &info) == 0 &&
        (info.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)) {
        *is_mountpoint_out = (info.stx_attributes & STATX_ATTR_MOUNT_ROOT) != 0;
        return true;
    }
#endif
    struct stat st;
    if (fstatat(dirfd, path, &st, 0) != 0) {
        bx_diag(diag, "cannot stat %s '%s': %s", label, path, strerror(errno));
        return false;
    }

    size_t path_len = strlen(path);
    char* parent = xmalloc(path_len + 4);
    memcpy(parent, path, path_len);
    memcpy(parent + path_len, "/..", 4);

    struct stat parent_st;
    if (fstatat(dirfd, parent, &parent_st, 0) != 0) {
        bx_diag(diag, "cannot stat parent of %s '%s': %s", label, path, strerror(errno));
        free(parent);
        return false;
    }

    free(parent);

    *is_mountpoint_out = (st.st_dev != parent_st.st_dev) || (st.st_ino == parent_st.st_ino);
    return true;
}

static bool bx_switch_root_enter_new_root(const char* new_root, struct bx_diag_ctx* diag) {
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

    int fd = bx_fd_open_cloexec(new_root, O_PATH, 0);
    struct stat st, old;
    bool ok = false;
    if (fd < 0 || fstat(fd, &st) != 0) {
        bx_diag(diag, "cannot access NEW_ROOT '%s': %s", new_root, strerror(errno));
        goto done;
    }
    if (!S_ISDIR(st.st_mode)) {
        bx_diag(diag, "NEW_ROOT '%s' is not a directory", new_root);
        goto done;
    }
    if (stat("/", &old) != 0) {
        bx_diag(diag, "cannot access current root: %s", strerror(errno));
        goto done;
    }
    if (st.st_dev == old.st_dev && st.st_ino == old.st_ino) {
        bx_diag(diag, "NEW_ROOT must not be '/' for initramfs handoff");
        goto done;
    }

    bool is_mountpoint = false;
    if (!bx_switch_root_path_is_mountpoint(fd, ".", &is_mountpoint, diag, "NEW_ROOT"))
        goto done;
    if (!is_mountpoint) {
        bx_diag(diag, "NEW_ROOT '%s' is not a mount point", new_root);
        goto done;
    }

    /* Keep the checked mount as cwd for preflight and for MS_MOVE. */
    if (fchdir(fd) != 0) {
        bx_diag(diag, "cannot change directory to NEW_ROOT '%s': %s", new_root, strerror(errno));
        goto done;
    }
    ok = true;
done:
    bx_fd_cleanup(&fd);
    return ok;
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

static bool bx_switch_root_validate_init_path(const char* init_path, struct bx_diag_ctx* diag) {
    char* candidate = bx_path_join_root_relative(".", init_path);

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

static bool bx_switch_root_validate_init_command(const char* init_command, struct bx_diag_ctx* diag) {
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
        char* relative_candidate = bx_path_join_root_relative(directory, init_command);
        char* candidate = bx_path_join_root_relative(".", relative_candidate);
        found = bx_switch_root_candidate_is_executable(candidate);
        free(candidate);
        free(relative_candidate);

        if (found || separator == NULL) {
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
        return bx_switch_root_validate_init_path(init_target, diag);
    }

    return bx_switch_root_validate_init_command(init_target, diag);
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
    if (!bx_switch_root_path_is_mountpoint(AT_FDCWD, source_path, &source_is_mountpoint, diag, "pseudo-filesystem path")) {
        return false;
    }

    if (!source_is_mountpoint) {
        return true;
    }

    struct stat target_st;
    if (lstat(target_relpath, &target_st) != 0) {
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

/* A persistent old root is a supported handoff, not a cleanup target. */
static int open_initramfs(struct bx_diag_ctx* diag) {
    struct statfs fs;
    struct stat marker, old, next;
    int fd = bx_fd_open_cloexec("/", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0 || fstatfs(fd, &fs) < 0 || fstat(fd, &old) < 0 || stat(".", &next) < 0) {
        bx_diag(diag, "skipping old root cleanup: %s", strerror(errno));
    }
    else if ((fs.f_type == RAMFS_MAGIC || fs.f_type == TMPFS_MAGIC) && old.st_dev != next.st_dev &&
             bx_fd_fstatat_child_nofollow(fd, "init", &marker) == 0 && S_ISREG(marker.st_mode) &&
             marker.st_dev == old.st_dev) {
        /* A relocated bind of old-root data would still alias the cleanup tree. */
        for (size_t i = 0; i < sizeof(bx_switch_root_pseudo_mounts) / sizeof(bx_switch_root_pseudo_mounts[0]); i++) {
            if (stat(bx_switch_root_pseudo_mounts[i].target_relpath, &next) == 0) {
                if (next.st_dev != old.st_dev)
                    continue;
            }
            else if (errno == ENOENT)
                continue;
            bx_diag(diag, "skipping old root cleanup: cannot isolate %s", bx_switch_root_pseudo_mounts[i].source_path);
            close(fd);
            return -1;
        }
        return fd;
    }
    bx_fd_cleanup(&fd);
    return -1;
}

/* Consume fd. A retained mount also retains all of its ancestor directories. */
static bool empty_initramfs(int fd, unsigned depth, struct bx_diag_ctx* diag) {
    DIR* dir = fdopendir(fd);
    if (!dir) {
        bx_diag(diag, "cannot read old root directory: %s", strerror(errno));
        close(fd);
        return false;
    }
    bool empty = true;
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(dir);
        if (!entry) {
            if (errno) {
                bx_diag(diag, "cannot scan old root: %s", strerror(errno));
                empty = false;
            }
            break;
        }
        const char* name = entry->d_name;
        if (bx_path_is_dot_or_dotdot(name))
            continue;
        int child = bx_openat_resolve(fd, name, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0,
                                      RESOLVE_NO_XDEV | RESOLVE_NO_SYMLINKS);
        if (child >= 0) {
            if (depth == 128) {
                close(child);
                bx_diag(diag, "old root directory nesting limit reached at '%s'", name);
                empty = false;
                continue;
            }
            if (!empty_initramfs(child, depth + 1, diag)) {
                empty = false;
                continue;
            }
        }
        else if (errno == EXDEV) {
            empty = false;
            continue;
        }
        else if (errno != ENOTDIR && errno != ELOOP) {
            bx_diag(diag, "cannot inspect old root entry '%s': %s", name, strerror(errno));
            empty = false;
            break;
        }
        if (bx_fd_unlinkat_child(fd, name, child >= 0 ? AT_REMOVEDIR : 0) < 0 && errno != ENOENT) {
            bx_diag(diag, "cannot remove old root entry '%s': %s", name, strerror(errno));
            empty = false;
        }
    }
    closedir(dir);
    return empty;
}

static int bx_switch_root_exec_init(const struct bx_switch_root_options* options, char** argv, struct bx_diag_ctx* diag) {
    char** init_argv = argv + options->init_index;

    int exec_error = bx_child_exec_argv_exact_or_path(init_argv);
    bx_diag(diag, "failed to exec INIT '%s': %s", init_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_switch_root_run(int argc, char** argv) {
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

    if (!bx_switch_root_enter_new_root(options.new_root, &diag)) {
        return 1;
    }

    if (!bx_switch_root_validate_init_target(&options, argc, argv, &diag)) {
        return 1;
    }

    for (size_t i = 0; i < sizeof(bx_switch_root_pseudo_mounts) / sizeof(bx_switch_root_pseudo_mounts[0]); i++) {
        const struct bx_switch_root_pseudo_mount* pseudo = &bx_switch_root_pseudo_mounts[i];
        if (!bx_switch_root_move_one_pseudo_mount(pseudo->source_path, pseudo->target_relpath, options.new_root, &diag)) {
            return 1;
        }
    }

    int old_fd = open_initramfs(&diag);
    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0) {
        bx_diag(&diag, "cannot move NEW_ROOT '%s' over '/': %s", options.new_root, strerror(errno));
        bx_fd_cleanup(&old_fd);
        return 1;
    }

    if (chroot(".") != 0) {
        bx_diag(&diag, "cannot chroot into NEW_ROOT '%s': %s", options.new_root, strerror(errno));
        bx_fd_cleanup(&old_fd);
        return 1;
    }

    if (chdir("/") != 0) {
        bx_diag(&diag, "cannot change working directory to '/': %s", strerror(errno));
        bx_fd_cleanup(&old_fd);
        return 1;
    }

    if (old_fd >= 0)
        (void)empty_initramfs(old_fd, 0, &diag);
    return bx_switch_root_exec_init(&options, argv, &diag);
}

int bx_switch_root_main(int argc, char** argv) {
    return bx_switch_root_run(argc, argv);
}
