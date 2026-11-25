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
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "lib/fd_ops.h"
#include "lib/option_scan.h"
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
static int modinfo_command(int argc, char** argv, const char** operands) {
    struct bx_diag_ctx diag = {.progname = "modinfo"};
    static const struct bx_option_spec options[] = {
        {'F', 'F', "field", BX_OPTION_REQUIRED_VALUE},
        {'b', 'b', "basedir", BX_OPTION_REQUIRED_VALUE},
        {'k', 'k', "set-version", BX_OPTION_REQUIRED_VALUE},
    };
    const char* field = NULL;
    const char* base = "/";
    const char* release = NULL;
    size_t count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ABBREVIATE,
                          options, sizeof(options) / sizeof(options[0]));
    for (;;) {
        struct bx_option_result result = bx_option_next(&cursor);
        if (result.event == BX_OPTION_END)
            break;
        if (result.event == BX_OPTION_OPERAND) {
            operands[count++] = result.token;
            continue;
        }
        if (result.event != BX_OPTION_MATCH) {
            if (result.event == BX_OPTION_MISSING_VALUE)
                bx_diag(&diag, "option requires an argument -- '%c'", result.spec->short_name);
            else if (result.short_name)
                bx_diag(&diag, "invalid option -- '%c'", result.short_name);
            else
                bx_diag(&diag, "unrecognized option '%s'", result.token);
            return 1;
        }
        switch (result.spec->id) {
        case 'F':
            if (*result.value == '\0') {
                bx_diag(&diag, "field must not be empty");
                return 1;
            }
            field = result.value;
            break;
        case 'b':
            if (*result.value == '\0') {
                bx_diag(&diag, "base directory must not be empty");
                return 1;
            }
            base = result.value;
            break;
        case 'k':
            if (!bx_module_release_valid(result.value)) {
                bx_diag(&diag, "invalid kernel release '%s'", result.value);
                return 1;
            }
            release = result.value;
            break;
        }
    }
    if (count == 0) {
        usage(stderr, diag.progname);
        return 1;
    }
    int status = 0;
    struct bx_module_tree tree = {.fd = -1};
    bool tried_tree = false;
    int tree_error = 0;
    for (size_t i = 0; i < count; i++) {
        const char* arg = operands[i];
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
}
#endif

int bx_modinfo_main(int argc, char** argv) {
    int handled = bx_cli_maybe_handle_help_or_version(argc, argv, "modinfo", "-h", "-V", usage);
    if (handled >= 0)
        return handled;
#ifdef __linux__
    const char** operands = NULL;
    if (argc > 1) {
        operands = calloc((size_t)argc - 1, sizeof(*operands));
        if (!operands) {
            struct bx_diag_ctx diag = {.progname = "modinfo"};
            bx_perror_path(&diag, "module operands");
            return 1;
        }
    }
    int status = modinfo_command(argc, argv, operands);
    free(operands);
    return status;
#else
    struct bx_diag_ctx diag = {.progname = "modinfo"};
    bx_diag(&diag, "Linux module metadata is not supported on this platform");
    return 1;
#endif
}
