#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"

enum bx_pwd_mode {
    BX_PWD_MODE_LOGICAL = 0,
    BX_PWD_MODE_PHYSICAL,
};

struct bx_pwd_options {
    const char* progname;
    enum bx_pwd_mode mode;
    bool show_help;
    bool show_version;
    bool extra_operand;
};

static void bx_pwd_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]...\n", progname);
    fprintf(stream, "Print the full filename of the current working directory.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -L, --logical   use PWD from environment, even if it contains symlinks\n");
    fprintf(stream, "  -P, --physical  resolve all symlinks (default)\n");
    fprintf(stream, "      --help      display this help and exit\n");
    fprintf(stream, "      --version   output version information and exit\n");
}

static bool bx_pwd_parse_options(int argc, char** argv, struct bx_pwd_options* options, struct bx_diag_ctx* diag) {
    static const struct bx_option_spec specs[] = {
        {'L', 'L', "logical", BX_OPTION_NO_VALUE},
        {'P', 'P', "physical", BX_OPTION_NO_VALUE},
        {1, 0, "help", BX_OPTION_NO_VALUE},
        {2, 0, "version", BX_OPTION_NO_VALUE},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "pwd");
    options->mode = BX_PWD_MODE_PHYSICAL;
    diag->progname = options->progname;

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            options->extra_operand = true;
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            if (option.short_name)
                bx_diag(diag, "invalid option -- '%c'", option.short_name);
            else
                bx_diag(diag, "unrecognized option '%s'", option.token);
            return false;
        }
        switch (option.spec->id) {
            case 'L':
                options->mode = BX_PWD_MODE_LOGICAL;
                break;
            case 'P':
                options->mode = BX_PWD_MODE_PHYSICAL;
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            default:
                return false;
        }
    }

    return true;
}

static bool bx_pwd_contains_only_normal_components(const char* path) {
    if (path == NULL || path[0] != '/') {
        return false;
    }

    const char* cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/') {
            cursor++;
        }

        if (*cursor == '\0') {
            break;
        }

        const char* component_start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }

        size_t component_len = (size_t)(cursor - component_start);
        if ((component_len == 1u && component_start[0] == '.') || (component_len == 2u && component_start[0] == '.' && component_start[1] == '.')) {
            return false;
        }
    }

    return true;
}

static bool bx_pwd_logical_path_matches_cwd(const char* logical_path) {
    struct stat cwd_stat;
    struct stat path_stat;

    if (stat(".", &cwd_stat) != 0) {
        return false;
    }

    if (stat(logical_path, &path_stat) != 0) {
        return false;
    }

    return (cwd_stat.st_dev == path_stat.st_dev) && (cwd_stat.st_ino == path_stat.st_ino);
}

static const char* bx_pwd_get_logical_path(void) {
    const char* logical_path = getenv("PWD");
    if (logical_path == NULL) {
        return NULL;
    }

    if (!bx_pwd_contains_only_normal_components(logical_path)) {
        return NULL;
    }

    if (!bx_pwd_logical_path_matches_cwd(logical_path)) {
        return NULL;
    }

    return logical_path;
}

static char* bx_pwd_getcwd_dup(void) {
    size_t size = 128u;
    char* cwd = xmalloc(size);

    while (getcwd(cwd, size) == NULL) {
        if (errno != ERANGE) {
            free(cwd);
            return NULL;
        }

        size *= 2u;
        cwd = xrealloc(cwd, size);
    }

    return cwd;
}

int bx_pwd_main(int argc, char** argv) {
    struct bx_pwd_options options;
    struct bx_diag_ctx diag = {.progname = "pwd", .exit_status = 0};

    if (!bx_pwd_parse_options(argc, argv, &options, &diag)) {
        return 1;
    }

    if (options.show_help) {
        bx_pwd_print_help(stdout, options.progname);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    if (options.extra_operand) {
        fprintf(stderr, "%s: ignoring non-option arguments\n", options.progname);
    }

    const char* output_path = NULL;
    char* physical_path = NULL;

    if (options.mode == BX_PWD_MODE_LOGICAL) {
        output_path = bx_pwd_get_logical_path();
    }

    if (output_path == NULL) {
        physical_path = bx_pwd_getcwd_dup();
        if (physical_path == NULL) {
            fprintf(stderr, "%s: failed to determine current directory: %s\n", options.progname, strerror(errno));
            return 1;
        }

        output_path = physical_path;
    }

    printf("%s\n", output_path);
    free(physical_path);
    return 0;
}
