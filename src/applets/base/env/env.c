#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

extern char** environ;

struct bx_env_options {
    const char* progname;
    bool ignore_environment;
    bool zero_terminated;
    bool show_help;
    bool show_version;
    const char** unset_names;
    size_t unset_count;
};

static void bx_env_print_help(FILE* stream, const char* progname) {
    fprintf(stream, "Usage: %s [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]\n", progname);
    fprintf(stream, "Set each NAME to VALUE in the environment and run COMMAND.\n");
    fprintf(stream, "With no COMMAND, print the resulting environment.\n");
    fprintf(stream, "\n");
    fprintf(stream, "  -i, --ignore-environment  start with an empty environment\n");
    fprintf(stream, "  -0, --null                end each output line with NUL, not newline\n");
    fprintf(stream, "  -u, --unset=NAME          remove variable NAME from the environment\n");
    fprintf(stream, "      --help                display this help and exit\n");
    fprintf(stream, "      --version             output version information and exit\n");
}

static void bx_env_options_cleanup(struct bx_env_options* options) {
    free(options->unset_names);
    options->unset_names = NULL;
    options->unset_count = 0;
}

static bool bx_env_is_valid_name(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    return strchr(name, '=') == NULL;
}

static bool bx_env_add_unset_name(struct bx_env_options* options, const char* name, struct bx_diag_ctx* diag) {
    if (!bx_env_is_valid_name(name)) {
        bx_diag(diag, "invalid variable name '%s'", (name != NULL) ? name : "");
        return false;
    }

    const char** grown = xrealloc(options->unset_names, (options->unset_count + 1u) * sizeof(*grown));
    options->unset_names = grown;
    options->unset_names[options->unset_count++] = name;
    return true;
}

static bool bx_env_parse_options(int argc, char** argv, struct bx_env_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"ignore-environment", no_argument, NULL, 'i'},
        {"null", no_argument, NULL, '0'},
        {"unset", required_argument, NULL, 'u'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "env");
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int c = getopt_long(argc, argv, "+0iu:", long_options, NULL);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '0':
                options->zero_terminated = true;
                break;
            case 'i':
                options->ignore_environment = true;
                break;
            case 'u':
                if (!bx_env_add_unset_name(options, optarg, diag)) {
                    return false;
                }
                break;
            case 1:
                options->show_help = true;
                return true;
            case 2:
                options->show_version = true;
                return true;
            case '?':
                bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    if (*first_operand < argc && strcmp(argv[*first_operand], "-") == 0) {
        options->ignore_environment = true;
        (*first_operand)++;
    }

    return true;
}

static bool bx_env_apply_option_environment(const struct bx_env_options* options, struct bx_diag_ctx* diag) {
    if (options->ignore_environment) {
        static char* empty_environment[] = {NULL};
        environ = empty_environment;
    }

    for (size_t i = 0; i < options->unset_count; i++) {
        const char* name = options->unset_names[i];
        if (unsetenv(name) != 0) {
            bx_diag(diag, "failed to unset '%s': %s", name, strerror(errno));
            return false;
        }
    }

    return true;
}

static bool bx_env_apply_assignment(const char* assignment, struct bx_diag_ctx* diag) {
    const char* equals = strchr(assignment, '=');
    if (equals == NULL) {
        return false;
    }

    size_t name_len = (size_t)(equals - assignment);
    char* name = xmalloc(name_len + 1u);
    memcpy(name, assignment, name_len);
    name[name_len] = '\0';

    if (!bx_env_is_valid_name(name)) {
        bx_diag(diag, "invalid variable name '%s'", name);
        free(name);
        return false;
    }

    const char* value = equals + 1;
    if (setenv(name, value, 1) != 0) {
        bx_diag(diag, "failed to set '%s': %s", name, strerror(errno));
        free(name);
        return false;
    }

    free(name);
    return true;
}

static bool bx_env_print_environment(bool zero_terminated, struct bx_diag_ctx* diag) {
    int delimiter = zero_terminated ? '\0' : '\n';

    for (char** entry = environ; entry != NULL && *entry != NULL; entry++) {
        if (fputs(*entry, stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        if (fputc(delimiter, stdout) == EOF) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
    }

    if (fflush(stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

static int bx_env_execute_command(char** command_argv, struct bx_diag_ctx* diag) {
    execvp(command_argv[0], command_argv);

    int exec_error = errno;
    bx_diag(diag, "%s: %s", command_argv[0], strerror(exec_error));
    if (exec_error == ENOENT) {
        return 127;
    }
    return 126;
}

int bx_env_main(int argc, char** argv) {
    struct bx_env_options options;
    struct bx_diag_ctx diag = {
        .progname = "env",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_env_parse_options(argc, argv, &options, &first_operand, &diag)) {
        bx_env_options_cleanup(&options);
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_env_print_help(stdout, options.progname);
        bx_env_options_cleanup(&options);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_env_options_cleanup(&options);
        return 0;
    }

    if (!bx_env_apply_option_environment(&options, &diag)) {
        bx_env_options_cleanup(&options);
        return diag.exit_status;
    }

    int index = first_operand;
    while (index < argc && strchr(argv[index], '=') != NULL) {
        if (!bx_env_apply_assignment(argv[index], &diag)) {
            bx_env_options_cleanup(&options);
            return diag.exit_status;
        }
        index++;
    }

    if (index >= argc) {
        if (!bx_env_print_environment(options.zero_terminated, &diag)) {
            bx_env_options_cleanup(&options);
            return diag.exit_status;
        }
        bx_env_options_cleanup(&options);
        return 0;
    }

    int rc = bx_env_execute_command(argv + index, &diag);
    bx_env_options_cleanup(&options);
    return rc;
}
