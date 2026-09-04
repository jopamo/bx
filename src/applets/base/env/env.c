#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "applets.h"
#include "applets/base/env/env_signal.h"
#include "applets/base/env/env_split.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/args_common.h"
#include "lib/child_runner.h"
#include "lib/cli_common.h"
#include "lib/env_ops.h"
#include "lib/output_quote.h"

enum {
    BX_ENV_EXIT_INTERNAL = 125,
    BX_ENV_EXIT_CANNOT_INVOKE = 126,
    BX_ENV_EXIT_NOT_FOUND = 127,
};

enum {
    BX_ENV_OPT_DEFAULT_SIGNAL = UCHAR_MAX + 1,
    BX_ENV_OPT_IGNORE_SIGNAL,
    BX_ENV_OPT_BLOCK_SIGNAL,
    BX_ENV_OPT_LIST_SIGNAL_HANDLING,
    BX_ENV_OPT_HELP,
    BX_ENV_OPT_VERSION,
};

struct bx_env_options {
    const char *progname;
    bool ignore_environment;
    bool zero_terminated;
    bool debug;
    bool show_help;
    bool show_version;
    char *argv0;
    const char *new_directory;
    const char **unset_names;
    size_t unset_count;
    struct bx_env_signal_policy signals;
    struct bx_env_split_result *splits;
    size_t split_count;
};

static void bx_env_print_help(FILE *stream, const char *progname) {
    fprintf(
        stream,
        "Usage: %s [OPTION]... [-] [NAME=VALUE]... [COMMAND [ARG]...]\n",
        progname);
    fputs("Set each NAME to VALUE in the environment and run COMMAND.\n", stream);
    fputs("\n", stream);
    fputs("Mandatory arguments to long options are mandatory for short options too.\n", stream);
    fputs("  -a, --argv0=ARG\n", stream);
    fputs("         pass ARG as the zeroth argument of COMMAND\n", stream);
    fputs("  -i, --ignore-environment\n", stream);
    fputs("         start with an empty environment\n", stream);
    fputs("  -0, --null\n", stream);
    fputs("         end each output line with NUL, not newline\n", stream);
    fputs("  -u, --unset=NAME\n", stream);
    fputs("         remove variable from the environment\n", stream);
    fputs("  -C, --chdir=DIR\n", stream);
    fputs("         change working directory to DIR\n", stream);
    fputs("  -S, --split-string=S\n", stream);
    fputs("         process and split S into separate arguments;\n", stream);
    fputs("         used to pass multiple arguments on shebang lines\n", stream);
    fputs("      --block-signal[=SIG]\n", stream);
    fputs("         block delivery of SIG signal(s) to COMMAND\n", stream);
    fputs("      --default-signal[=SIG]\n", stream);
    fputs("         reset handling of SIG signal(s) to the default\n", stream);
    fputs("      --ignore-signal[=SIG]\n", stream);
    fputs("         set handling of SIG signal(s) to do nothing\n", stream);
    fputs("      --list-signal-handling\n", stream);
    fputs("         list non default signal handling to standard error\n", stream);
    fputs("  -v, --debug\n", stream);
    fputs("         print verbose information for each processing step\n", stream);
    fputs("      --help\n", stream);
    fputs("         display this help and exit\n", stream);
    fputs("      --version\n", stream);
    fputs("         output version information and exit\n", stream);
    fputs("\n", stream);
    fputs("A mere - implies -i.  If no COMMAND, print the resulting environment.\n", stream);
    fputs("\n", stream);
    fputs("SIG may be a signal name like 'PIPE', or a signal number like '13'.\n", stream);
    fputs("Without SIG, all known signals are included.  Multiple signals can be\n", stream);
    fputs("comma-separated.  An empty SIG argument is a no-op.\n", stream);
    fputs("\n", stream);
    fputs("Exit status:\n", stream);
    fputs("  125  if the env command itself fails\n", stream);
    fputs("  126  if COMMAND is found but cannot be invoked\n", stream);
    fputs("  127  if COMMAND cannot be found\n", stream);
    fputs("  -    the exit status of COMMAND otherwise\n", stream);
}

static void bx_env_options_cleanup(struct bx_env_options *options) {
    free(options->unset_names);
    for (size_t index = 0; index < options->split_count; index++)
        bx_env_split_result_destroy(&options->splits[index]);
    free(options->splits);
    *options = (struct bx_env_options){0};
}

static bool bx_env_add_unset_name(
    struct bx_env_options *options,
    const char *name) {
    if (options->unset_count == SIZE_MAX / sizeof(*options->unset_names))
        return false;
    const char **names = realloc(
        options->unset_names,
        (options->unset_count + 1u) * sizeof(*names));
    if (names == NULL)
        return false;
    options->unset_names = names;
    options->unset_names[options->unset_count++] = name;
    return true;
}

static bool bx_env_add_split(
    struct bx_env_options *options,
    struct bx_env_split_result *split) {
    if (options->split_count == SIZE_MAX / sizeof(*options->splits))
        return false;
    struct bx_env_split_result *splits = realloc(
        options->splits,
        (options->split_count + 1u) * sizeof(*splits));
    if (splits == NULL)
        return false;
    options->splits = splits;
    options->splits[options->split_count++] = *split;
    *split = (struct bx_env_split_result){0};
    return true;
}

static bool bx_env_option_failure(
    struct bx_diag_ctx *diag,
    int option,
    int current_optind,
    int argc,
    char **argv,
    bool missing_argument) {
    if (missing_argument &&
        current_optind > 0 &&
        current_optind <= argc &&
        argv[current_optind - 1] != NULL &&
        strncmp(argv[current_optind - 1], "--", 2) == 0) {
        bx_diag(
            diag,
            "option '%s' requires an argument",
            argv[current_optind - 1]);
    } else if (missing_argument) {
        bx_cli_diag_option_requires_arg(
            diag, option, current_optind, argc, argv);
    } else {
        bx_cli_diag_unrecognized_option(
            diag, option, current_optind, argc, argv);
    }
    bx_cli_print_try_help(diag->progname);
    return false;
}

static bool bx_env_parse_options(
    int *argc_pointer,
    char ***argv_pointer,
    struct bx_env_options *options,
    int *first_operand,
    struct bx_diag_ctx *diag) {
    static const struct option long_options[] = {
        {"argv0", required_argument, NULL, 'a'},
        {"ignore-environment", no_argument, NULL, 'i'},
        {"null", no_argument, NULL, '0'},
        {"unset", required_argument, NULL, 'u'},
        {"chdir", required_argument, NULL, 'C'},
        {"default-signal", optional_argument, NULL, BX_ENV_OPT_DEFAULT_SIGNAL},
        {"ignore-signal", optional_argument, NULL, BX_ENV_OPT_IGNORE_SIGNAL},
        {"block-signal", optional_argument, NULL, BX_ENV_OPT_BLOCK_SIGNAL},
        {"list-signal-handling", no_argument, NULL,
         BX_ENV_OPT_LIST_SIGNAL_HANDLING},
        {"debug", no_argument, NULL, 'v'},
        {"split-string", required_argument, NULL, 'S'},
        {"help", no_argument, NULL, BX_ENV_OPT_HELP},
        {"version", no_argument, NULL, BX_ENV_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    int argc = *argc_pointer;
    char **argv = *argv_pointer;
    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname(
        argc > 0 ? argv[0] : NULL, "env");
    diag->progname = options->progname;
    bx_env_signal_policy_init(&options->signals);
    bx_args_getopt_reset();

    for (;;) {
        int option = bx_args_getopt_long(
            argc,
            argv,
            "+:a:C:iS:u:v0 \t\n\v\f\r",
            long_options,
            NULL);
        if (option == -1)
            break;

        switch (option) {
            case 'a':
                options->argv0 = optarg;
                break;
            case 'C':
                options->new_directory = optarg;
                break;
            case 'i':
                options->ignore_environment = true;
                break;
            case 'S': {
                struct bx_env_split_result split;
                if (!bx_env_split_parse(
                        optarg,
                        argc,
                        argv,
                        optind,
                        options->debug,
                        diag,
                        &split)) {
                    return false;
                }
                argc = split.argc;
                argv = split.argv;
                if (!bx_env_add_split(options, &split)) {
                    bx_env_split_result_destroy(&split);
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
                bx_args_getopt_reset();
                break;
            }
            case 'u':
                if (!bx_env_add_unset_name(options, optarg)) {
                    bx_diag(diag, "memory exhausted");
                    return false;
                }
                break;
            case 'v':
                options->debug = true;
                break;
            case '0':
                options->zero_terminated = true;
                break;
            case BX_ENV_OPT_DEFAULT_SIGNAL:
                if (!bx_env_signal_parse_action(
                        &options->signals, optarg, true, diag) ||
                    !bx_env_signal_parse_block(
                        &options->signals, optarg, false, diag)) {
                    return false;
                }
                break;
            case BX_ENV_OPT_IGNORE_SIGNAL:
                if (!bx_env_signal_parse_action(
                        &options->signals, optarg, false, diag)) {
                    return false;
                }
                break;
            case BX_ENV_OPT_BLOCK_SIGNAL:
                if (!bx_env_signal_parse_block(
                        &options->signals, optarg, true, diag)) {
                    return false;
                }
                break;
            case BX_ENV_OPT_LIST_SIGNAL_HANDLING:
                options->signals.report_handling = true;
                break;
            case BX_ENV_OPT_HELP:
                options->show_help = true;
                *argc_pointer = argc;
                *argv_pointer = argv;
                return true;
            case BX_ENV_OPT_VERSION:
                options->show_version = true;
                *argc_pointer = argc;
                *argv_pointer = argv;
                return true;
            case ' ':
            case '\t':
            case '\n':
            case '\v':
            case '\f':
            case '\r': {
                bx_diag(diag, "invalid option -- '%c'", option);
                bx_diag(
                    diag,
                    "use -[v]S to pass options in shebang lines");
                bx_cli_print_try_help(options->progname);
                return false;
            }
            case ':':
                return bx_env_option_failure(
                    diag, optopt, optind, argc, argv, true);
            case '?':
            default:
                return bx_env_option_failure(
                    diag, optopt, optind, argc, argv, false);
        }
    }

    *first_operand = optind;
    if (*first_operand < argc &&
        strcmp(argv[*first_operand], "-") == 0) {
        options->ignore_environment = true;
        (*first_operand)++;
    }
    *argc_pointer = argc;
    *argv_pointer = argv;
    return true;
}

static char *bx_env_quote_always(const char *text) {
    return bx_output_quote_dup(
        text, BX_OUTPUT_QUOTE_SHELL_ESCAPE_ALWAYS);
}

static void bx_env_debug_value(
    const char *prefix,
    const char *value) {
    char *quoted = bx_env_quote_always(value);
    fprintf(stderr, "%s%s\n", prefix, quoted);
    free(quoted);
}

static bool bx_env_valid_unset_name(const char *name) {
    return name != NULL &&
           name[0] != '\0' &&
           strchr(name, '=') == NULL;
}

static bool bx_env_apply_unsets(
    const struct bx_env_options *options,
    struct bx_env_vector *environment,
    struct bx_diag_ctx *diag) {
    for (size_t index = 0; index < options->unset_count; index++) {
        const char *name = options->unset_names[index];
        if (options->debug)
            fprintf(stderr, "unset:    %s\n", name);
        if (!bx_env_valid_unset_name(name)) {
            char *quoted = bx_env_quote_always(name);
            bx_diag(
                diag,
                "cannot unset %s: %s",
                quoted,
                strerror(EINVAL));
            free(quoted);
            return false;
        }
        bx_env_vector_unset(environment, name);
    }
    return true;
}

static bool bx_env_apply_assignment(
    struct bx_env_vector *environment,
    const char *assignment,
    bool debug,
    struct bx_diag_ctx *diag) {
    const char *equals = strchr(assignment, '=');
    if (equals == NULL)
        return false;
    if (debug)
        fprintf(stderr, "setenv:   %s\n", assignment);

    size_t name_length = (size_t)(equals - assignment);
    char *name = malloc(name_length + 1u);
    if (name == NULL) {
        bx_diag(diag, "memory exhausted");
        return false;
    }
    memcpy(name, assignment, name_length);
    name[name_length] = '\0';
    int error = bx_env_vector_set(
        environment, name, equals + 1);
    if (error != 0) {
        char *quoted = bx_env_quote_always(name);
        bx_diag(
            diag,
            "cannot set %s: %s",
            quoted,
            strerror(error));
        free(quoted);
        free(name);
        return false;
    }
    free(name);
    return true;
}

static bool bx_env_print_environment(
    const struct bx_env_vector *environment,
    bool zero_terminated,
    struct bx_diag_ctx *diag) {
    int delimiter = zero_terminated ? '\0' : '\n';
    for (char *const *entry = bx_env_vector_data(environment);
         entry != NULL && *entry != NULL;
         entry++) {
        if (!bx_cli_emit_delimited(*entry, delimiter, diag)) {
            return false;
        }
    }
    return bx_cli_flush_stdout(diag);
}

static int bx_env_execute_command(
    const char *program,
    char **command_argv,
    struct bx_env_vector *environment,
    const char *path,
    struct bx_diag_ctx *diag) {
    int exec_error = bx_child_exec_file_argv_envp(
        program,
        command_argv,
        bx_env_vector_data(environment),
        path);
    char *quoted = bx_env_quote_always(program);
    bx_diag(diag, "%s: %s", quoted, strerror(exec_error));
    free(quoted);
    if (exec_error == ENOENT)
        return BX_ENV_EXIT_NOT_FOUND;
    return BX_ENV_EXIT_CANNOT_INVOKE;
}

int bx_env_main(int argc, char **argv) {
    struct bx_env_options options;
    struct bx_diag_ctx diag = {
        .progname = "env",
        .exit_status = BX_ENV_EXIT_INTERNAL,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;
    if (!bx_env_parse_options(
            &argc, &argv, &options, &first_operand, &diag)) {
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
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

    struct bx_env_vector environment;
    int error;
    if (options.ignore_environment) {
        if (options.debug)
            fputs("cleaning environ\n", stderr);
        error = bx_env_vector_init_empty(&environment);
    } else {
        error = bx_env_vector_init_current(&environment);
    }
    if (error != 0) {
        bx_diag(&diag, "memory exhausted");
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }

    bool success = true;
    if (!options.ignore_environment) {
        success = bx_env_apply_unsets(
            &options, &environment, &diag);
    }

    int index = first_operand;
    while (success &&
           index < argc &&
           strchr(argv[index], '=') != NULL) {
        success = bx_env_apply_assignment(
            &environment, argv[index], options.debug, &diag);
        index++;
    }
    if (!success) {
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }

    bool command_specified = index < argc;
    if (options.zero_terminated && command_specified) {
        bx_diag(&diag, "cannot specify --null (-0) with command");
        bx_cli_print_try_help(options.progname);
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }
    if (options.new_directory != NULL && !command_specified) {
        bx_diag(&diag, "must specify command with --chdir (-C)");
        bx_cli_print_try_help(options.progname);
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }
    if (options.argv0 != NULL && !command_specified) {
        bx_diag(&diag, "must specify command with --argv0 (-a)");
        bx_cli_print_try_help(options.progname);
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }

    if (!command_specified) {
        bool printed = bx_env_print_environment(
            &environment,
            options.zero_terminated,
            &diag);
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return printed ? 0 : BX_ENV_EXIT_INTERNAL;
    }

    if (!bx_env_signal_apply(
            &options.signals, options.debug, &diag) ||
        (options.signals.report_handling &&
         !bx_env_signal_list(&diag))) {
        bx_env_vector_destroy(&environment);
        bx_env_options_cleanup(&options);
        return BX_ENV_EXIT_INTERNAL;
    }

    if (options.new_directory != NULL) {
        if (options.debug)
            bx_env_debug_value(
                "chdir:    ", options.new_directory);
        if (chdir(options.new_directory) != 0) {
            char *quoted = bx_env_quote_always(
                options.new_directory);
            bx_diag(
                &diag,
                "cannot change directory to %s: %s",
                quoted,
                strerror(errno));
            free(quoted);
            bx_env_vector_destroy(&environment);
            bx_env_options_cleanup(&options);
            return BX_ENV_EXIT_INTERNAL;
        }
    }

    char *program = argv[index];
    if (options.argv0 != NULL) {
        if (options.debug)
            bx_env_debug_value("argv0:     ", options.argv0);
        argv[index] = options.argv0;
    }
    if (options.debug) {
        fprintf(stderr, "executing: %s\n", program);
        for (int arg_index = index; arg_index < argc; arg_index++) {
            char prefix[64];
            snprintf(
                prefix,
                sizeof(prefix),
                "   arg[%d]= ",
                arg_index - index);
            bx_env_debug_value(prefix, argv[arg_index]);
        }
    }

    const char *path = bx_env_vector_get(&environment, "PATH");
    int result = bx_env_execute_command(
        program, argv + index, &environment, path, &diag);
    if (result == BX_ENV_EXIT_NOT_FOUND &&
        strpbrk(program, " \t\n\v\f\r") != NULL) {
        bx_diag(
            &diag,
            "use -[v]S to pass options in shebang lines");
    }
    bx_env_vector_destroy(&environment);
    bx_env_options_cleanup(&options);
    return result;
}
