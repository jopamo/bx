#define _GNU_SOURCE
#include <stdio.h>

#include "applets.h"
#include "bx/diag.h"
#include "lib/cli_common.h"

static void usage(FILE* out, const char* name) {
    fprintf(out, "Usage: %s [-F FIELD] [-b ROOT] [-k RELEASE] [--] MODULE...\n"
                 "Print metadata from Linux module files (plain, gzip, xz, or zstd)\n"
                 "or look up exact names under ROOT/lib/modules/RELEASE.\n"
                 "Use ./NAME for a file with a bare module name.\n", name);
}

#ifdef __linux__
#include <getopt.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"
#include "module_elf.h"
#include "module_index.h"

static bool print_field(const char* key, size_t len, const char* value, const char* field) {
    if (field != NULL)
        return strlen(field) != len || memcmp(field, key, len) != 0 || puts(value) != EOF;
    return printf("%.*s:%*s%s\n", (int)len, key, len < 15 ? (int)(15 - len) : 1, "", value) >= 0;
}

static bool print_records(const char* data, size_t len, const char* field, const char* module) {
    for (const char* p = data; p < data + len; p += strlen(p) + 1) {
        if (*p == '\0')
            continue;
        const char* key = p;
        if (module != NULL) {
            const char* dot = strchr(p, '.');
            if (!bx_module_name_matches(module, p, (size_t)(dot - p)))
                continue;
            key = dot + 1;
        }
        const char* eq = strchr(key, '=');
        if (!print_field(key, (size_t)(eq - key), eq + 1, field))
            return false;
    }
    return true;
}

static bool print_builtin(const char* name, const char* data, size_t len, const char* field) {
    char normalized[NAME_MAX + 1];
    size_t length = strlen(name);
    for (size_t i = 0; i < length; i++)
        normalized[i] = name[i] == '-' ? '_' : name[i];
    normalized[length] = '\0';
    if (!print_field("name", 4, normalized, field) ||
        !print_field("filename", 8, "(builtin)", field))
        return false;
    return print_records(data, len, field, name);
}
#endif

int bx_modinfo_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "modinfo", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
    struct bx_diag_ctx diag = {.progname = "modinfo"};
#ifdef __linux__
    static const struct option opts[] = {
        {"field", required_argument, NULL, 'F'},
        {"basedir", required_argument, NULL, 'b'},
        {"set-version", required_argument, NULL, 'k'},
        {NULL, 0, NULL, 0},
    };
    const char* field = NULL;
    const char* base = "/";
    const char* release = NULL;
    opterr = 0;
    optind = 1;
    for (int opt; (opt = getopt_long(argc, argv, ":F:b:k:", opts, NULL)) != -1;) {
        if (opt == 'F') {
            if (*optarg == '\0') {
                bx_diag(&diag, "field must not be empty");
                return 1;
            }
            field = optarg;
        } else if (opt == 'b') {
            if (*optarg == '\0') {
                bx_diag(&diag, "base directory must not be empty");
                return 1;
            }
            base = optarg;
        } else if (opt == 'k') {
            if (!bx_module_release_valid(optarg)) {
                bx_diag(&diag, "invalid kernel release '%s'", optarg);
                return 1;
            }
            release = optarg;
        } else {
            if (opt == ':')
                bx_cli_diag_option_requires_arg(&diag, optopt, optind, argc, argv);
            else
                bx_cli_diag_unrecognized_option(&diag, optopt, optind, argc, argv);
            return 1;
        }
    }
    if (optind == argc) {
        usage(stderr, diag.progname);
        return 1;
    }
    int status = 0;
    struct bx_module_tree tree = {.fd = -1};
    bool tried_tree = false;
    int tree_error = 0;
    for (int i = optind; i < argc; i++) {
        const char* arg = argv[i];
        char relative[PATH_MAX];
        bool named = bx_module_name_valid(arg);
        int fd;
        if (named) {
            if (!tried_tree) {
                tried_tree = true;
                if (bx_module_tree_open(&tree, base, release) < 0)
                    tree_error = errno;
            }
            if (tree_error != 0) {
                errno = tree_error;
                bx_perror_path(&diag, tree.path ? tree.path : "module tree");
                status = 1;
                continue;
            }
            int found = bx_module_tree_find(&tree, arg, relative, sizeof(relative));
            if (found < 0) {
                bx_perror_path(&diag, arg);
                status = 1;
                continue;
            }
            if (found == 1) {
                const char* data = "";
                size_t len = 0;
                if ((field == NULL || (strcmp(field, "filename") != 0 &&
                                       strcmp(field, "name") != 0)) &&
                    bx_module_tree_builtin_info(&tree, &data, &len) < 0) {
                    bx_perror_path(&diag, "modules.builtin.modinfo");
                    status = 1;
                    continue;
                }
                if (!print_builtin(arg, data, len, field) || fflush(stdout) == EOF)
                    goto output_error;
                continue;
            }
            fd = bx_module_tree_open_file(&tree, relative, O_RDONLY | O_NONBLOCK);
        } else {
            fd = bx_fd_open_cloexec(arg, O_RDONLY | O_NONBLOCK, 0);
        }
        if (fd < 0) {
            bx_perror_path(&diag, arg);
            status = 1;
            continue;
        }
        size_t size;
        unsigned char* file = bx_module_elf_read(fd, arg, &size, &diag);
        bx_fd_cleanup(&fd);
        if (file == NULL) {
            status = 1;
            continue;
        }
        const char* data;
        size_t len;
        bool printed = true;
        if (!bx_module_elf_info(file, size, &data, &len)) {
            bx_diag(&diag, "'%s': expected an ELF module with valid .modinfo", arg);
            status = 1;
        } else if (field != NULL && strcmp(field, "filename") == 0) {
            printed = (named ? printf("%s/%s\n", tree.path, relative) : puts(arg)) >= 0;
        } else {
            printed = (field != NULL ||
                       (named ? printf("filename:       %s/%s\n", tree.path, relative)
                              : printf("filename:       %s\n", arg)) >= 0) &&
                      print_records(data, len, field, NULL);
        }
        free(file);
        if (!printed || fflush(stdout) == EOF)
            goto output_error;
    }
    bx_module_tree_close(&tree);
    return status;
output_error:
    bx_perror_path(&diag, "standard output");
    bx_module_tree_close(&tree);
    return 1;
#else
    bx_diag(&diag, "Linux module metadata is not supported on this platform");
    return 1;
#endif
}
