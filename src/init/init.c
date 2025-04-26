#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "diag.h"

struct bx_init_options {
    const char* progname;
    bool show_help;
    bool show_version;
    bool mount_pseudo;
    int command_index;
};

struct bx_init_pseudo_mount {
    const char* source;
    const char* target;
    const char* fstype;
    const char* options;
};

static const struct bx_init_pseudo_mount bx_init_pseudo_mounts[] = {
    {"proc", "/proc", "proc", NULL},
    {"sysfs", "/sys", "sysfs", NULL},
    {"tmpfs", "/dev", "tmpfs", "mode=0755,nosuid"},
    {"tmpfs", "/run", "tmpfs", "mode=0755,nosuid,nodev"},
};

static const char* bx_init_progname(const char* argv0) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return "init";
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

static void bx_init_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... PROGRAM [ARG]...\n", progname);
    fprintf(stream, "Exec PROGRAM as the init payload process.\n");
    fprintf(stream, "\n");
    fprintf(stream, "This phase is intentionally minimal: it validates CLI shape and then\n");
    fprintf(stream, "execs the requested program directly.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -h, --help     display this help and exit\n");
    fprintf(stream, "  -m, --mount-pseudo  ensure /proc, /sys, /dev, and /run are mounted\n");
    fprintf(stream, "  -V, --version  output version information and exit\n");
}

static void bx_init_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static void bx_init_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

static bool bx_init_parse_options(int argc, char** argv, struct bx_init_options* options, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"mount-pseudo", no_argument, NULL, 'm'},
        {"version", no_argument, NULL, 'V'},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_init_progname((argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+hmV", long_options, NULL);
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

    if (optind >= argc) {
        bx_diag(diag, "missing operand: PROGRAM");
        return false;
    }

    options->command_index = optind;
    return true;
}

static int bx_init_exec_program(const struct bx_init_options* options, char** argv, struct bx_diag_ctx* diag) {
    char** command_argv = argv + options->command_index;
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "cannot execute '%s': %s", command_argv[0], strerror(exec_error));

    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
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

int bx_init_main(int argc, char** argv) {
    struct bx_init_options options;
    struct bx_diag_ctx diag = {
        .progname = "init",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_init_parse_options(argc, argv, &options, &diag)) {
        bx_init_print_try_help(options.progname);
        return 2;
    }

    if (options.show_help) {
        bx_init_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_init_print_version(options.progname);
        return 0;
    }

    if (options.mount_pseudo) {
        int mount_status = bx_init_mount_pseudo_filesystems(&diag);
        if (mount_status != 0) {
            return mount_status;
        }
    }

    return bx_init_exec_program(&options, argv, &diag);
}
