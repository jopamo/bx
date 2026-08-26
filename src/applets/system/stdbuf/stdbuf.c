#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/args_common.h"
#include "lib/child_runner.h"
#include "lib/cli_common.h"
#include "lib/preload_ops.h"
#include "lib/size_parse.h"

#ifndef BX_STDBUF_INSTALLED_PATH
#define BX_STDBUF_INSTALLED_PATH ""
#endif
#ifndef BX_STDBUF_INSTALL_SUBDIR
#define BX_STDBUF_INSTALL_SUBDIR ""
#endif

enum bx_stdbuf_parse_result {
    BX_STDBUF_PARSE_OK = 0,
    BX_STDBUF_PARSE_INVALID,
    BX_STDBUF_PARSE_NO_NUMBER,
    BX_STDBUF_PARSE_OVERFLOW,
};

struct bx_stdbuf_stream_mode {
    bool selected;
    bool line_buffered;
    size_t size;
};

struct bx_stdbuf_options {
    const char *progname;
    struct bx_stdbuf_stream_mode streams[3];
    bool show_help;
    bool show_version;
    bool suppress_try_help;
    int first_operand;
};

static void bx_stdbuf_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s OPTION... COMMAND\n", progname);
    fprintf(stream, "Run COMMAND, with modified buffering operations for its standard streams.\n\n");
    fprintf(stream, "Mandatory arguments to long options are mandatory for short options too.\n");
    fprintf(stream, "  -i, --input=MODE   adjust standard input stream buffering\n");
    fprintf(stream, "  -o, --output=MODE  adjust standard output stream buffering\n");
    fprintf(stream, "  -e, --error=MODE   adjust standard error stream buffering\n");
    fprintf(stream, "      --help         display this help and exit\n");
    fprintf(stream, "      --version      output version information and exit\n\n");
    fprintf(stream, "If MODE is 'L' the corresponding stream will be line buffered.\n");
    fprintf(stream, "This option is invalid with standard input.\n\n");
    fprintf(stream, "If MODE is '0' the corresponding stream will be unbuffered.\n\n");
    fprintf(stream, "Otherwise MODE is a number which may be followed by one of the following:\n");
    fprintf(stream, "KB 1000, K 1024, MB 1000*1000, M 1024*1024, and so on for G,T,P,E,Z,Y,R,Q.\n");
    fprintf(stream, "Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n");
    fprintf(stream, "In this case the corresponding stream will be fully buffered with the buffer\n");
    fprintf(stream, "size set to MODE bytes.\n\n");
    fprintf(stream, "NOTE: If COMMAND adjusts the buffering of its standard streams ('tee' does\n");
    fprintf(stream, "for example) then that will override corresponding changes by 'stdbuf'.\n");
    fprintf(stream, "Also some filters (like 'dd' and 'cat' etc.) don't use streams for I/O,\n");
    fprintf(stream, "and are thus unaffected by 'stdbuf' settings.\n\n");
    fprintf(stream, "Exit status:\n");
    fprintf(stream, "  125  if the stdbuf command itself fails\n");
    fprintf(stream, "  126  if COMMAND is found but cannot be invoked\n");
    fprintf(stream, "  127  if COMMAND cannot be found\n");
    fprintf(stream, "  -    the exit status of COMMAND otherwise\n");
}

static bool bx_stdbuf_ascii_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

static bool bx_stdbuf_suffix_syntax(
    const char *suffix,
    unsigned int *power_out,
    uintmax_t *base_out
) {
    if (suffix[0] == '\0') {
        *power_out = 0u;
        *base_out = 1u;
        return true;
    }

    unsigned int power = 0u;
    if (!bx_size_suffix_prefix_power(suffix[0], &power)) {
        return false;
    }

    uintmax_t base = 1024u;
    if (suffix[1] == '\0') {
        *power_out = power;
        *base_out = base;
        return true;
    }
    if (suffix[1] == 'B' && suffix[2] == '\0') {
        *power_out = power;
        *base_out = 1000u;
        return true;
    }
    if (suffix[1] == 'i' && suffix[2] == 'B' && suffix[3] == '\0') {
        *power_out = power;
        *base_out = base;
        return true;
    }
    return false;
}

static enum bx_stdbuf_parse_result bx_stdbuf_parse_size(const char *text, size_t *size_out) {
    const char *number = text;
    if (number[0] == '+') {
        number++;
    }

    const char *suffix = number;
    while (suffix[0] >= '0' && suffix[0] <= '9') {
        suffix++;
    }
    if (number[0] == '\0') {
        return BX_STDBUF_PARSE_NO_NUMBER;
    }

    unsigned int power = 0u;
    uintmax_t base = 1u;
    if (!bx_stdbuf_suffix_syntax(suffix, &power, &base)) {
        if (number[0] == '\0' || (suffix == number && text[0] != '-' && text[0] != '+')) {
            return BX_STDBUF_PARSE_NO_NUMBER;
        }
        return BX_STDBUF_PARSE_INVALID;
    }

    uintmax_t value = 1u;
    if (suffix != number) {
        size_t digits_len = (size_t)(suffix - number);
        char *digits = xmalloc(digits_len + 1u);
        memcpy(digits, number, digits_len);
        digits[digits_len] = '\0';
        bool parsed = bx_size_parse_uint(digits, &value);
        free(digits);
        if (!parsed) {
            return BX_STDBUF_PARSE_OVERFLOW;
        }
    }
    else if (text[0] == '+') {
        return BX_STDBUF_PARSE_NO_NUMBER;
    }

    uintmax_t scaled = value;
    if (value != 0u && power != 0u &&
        !bx_size_multiply_by_power_uint(value, base, power, &scaled)) {
        return BX_STDBUF_PARSE_OVERFLOW;
    }
    if (scaled > SIZE_MAX) {
        return BX_STDBUF_PARSE_OVERFLOW;
    }

    *size_out = (size_t)scaled;
    return BX_STDBUF_PARSE_OK;
}

static int bx_stdbuf_stream_index(int option_char) {
    switch (option_char) {
        case 'i':
            return 0;
        case 'o':
            return 1;
        case 'e':
            return 2;
        default:
            return -1;
    }
}

static bool bx_stdbuf_set_mode(
    struct bx_stdbuf_options *options,
    int option_char,
    const char *mode_text,
    struct bx_diag_ctx *diag
) {
    while (bx_stdbuf_ascii_space(mode_text[0])) {
        mode_text++;
    }

    int stream_index = bx_stdbuf_stream_index(option_char);
    struct bx_stdbuf_stream_mode *stream = &options->streams[stream_index];
    stream->selected = true;

    if (strcmp(mode_text, "L") == 0) {
        if (option_char == 'i') {
            bx_diag(diag, "line buffering standard input is meaningless");
            return false;
        }
        stream->line_buffered = true;
        stream->size = 0u;
        return true;
    }

    size_t size = 0u;
    enum bx_stdbuf_parse_result result = bx_stdbuf_parse_size(mode_text, &size);
    if (result != BX_STDBUF_PARSE_OK) {
        options->suppress_try_help = true;
        if (result == BX_STDBUF_PARSE_NO_NUMBER) {
            bx_diag(diag, "invalid mode '%s': Invalid argument", mode_text);
        }
        else if (result == BX_STDBUF_PARSE_OVERFLOW) {
            bx_diag(diag, "invalid mode '%s': Value too large for data type", mode_text);
        }
        else {
            bx_diag(diag, "invalid mode '%s'", mode_text);
        }
        return false;
    }

    stream->line_buffered = false;
    stream->size = size;
    return true;
}

static bool bx_stdbuf_parse_options(
    int argc,
    char **argv,
    struct bx_stdbuf_options *options,
    struct bx_diag_ctx *diag
) {
    static const struct option long_options[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"error", required_argument, NULL, 'e'},
        {"help", no_argument, NULL, 1},
        {"version", no_argument, NULL, 2},
        {NULL, 0, NULL, 0},
    };

    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "stdbuf");
    diag->progname = options->progname;
    bx_args_getopt_reset();

    while (true) {
        int option_char = bx_args_getopt_long(argc, argv, "+i:o:e:", long_options, NULL);
        if (option_char == -1) {
            break;
        }

        switch (option_char) {
            case 'i':
            case 'o':
            case 'e':
                if (!bx_stdbuf_set_mode(options, option_char, optarg, diag)) {
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
                if (optopt == 'i' || optopt == 'o' || optopt == 'e') {
                    const char *option_text =
                        (optind > 0 && optind <= argc) ? argv[optind - 1] : NULL;
                    if (option_text != NULL && option_text[0] == '-' && option_text[1] == '-') {
                        bx_diag(diag, "option '%s' requires an argument", option_text);
                    }
                    else {
                        bx_cli_diag_option_requires_arg(diag, optopt, optind, argc, argv);
                    }
                }
                else {
                    bx_cli_diag_unrecognized_option(diag, optopt, optind, argc, argv);
                }
                return false;
            default:
                return false;
        }
    }

    options->first_operand = optind;
    return true;
}

static bool bx_stdbuf_set_stream_environment(
    const struct bx_stdbuf_options *options,
    struct bx_diag_ctx *diag
) {
    static const char *const names[] = {"_STDBUF_I", "_STDBUF_O", "_STDBUF_E"};
    char value[64];

    for (size_t index = 0u; index < 3u; index++) {
        const struct bx_stdbuf_stream_mode *stream = &options->streams[index];
        if (!stream->selected) {
            continue;
        }

        if (stream->line_buffered) {
            memcpy(value, "L", 2u);
        }
        else {
            (void)snprintf(value, sizeof(value), "%zu", stream->size);
        }
        if (setenv(names[index], value, 1) != 0) {
            bx_diag(diag, "failed to update the environment with '%s=%s': %s",
                    names[index], value, strerror(errno));
            return false;
        }
    }
    return true;
}

static bool bx_stdbuf_set_preload_environment(const char *module, struct bx_diag_ctx *diag) {
    char *value = NULL;
    int error = bx_preload_append_environment("LD_PRELOAD", module, &value);
    if (error != 0) {
        bx_diag(diag, "failed to update the environment with 'LD_PRELOAD=%s': %s",
                value, strerror(error));
        free(value);
        return false;
    }
    free(value);
    return true;
}

static bool bx_stdbuf_has_selected_mode(const struct bx_stdbuf_options *options) {
    return options->streams[0].selected || options->streams[1].selected || options->streams[2].selected;
}

int bx_stdbuf_main(int argc, char **argv) {
    struct bx_stdbuf_options options;
    struct bx_diag_ctx diag = {
        .progname = "stdbuf",
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };

    if (!bx_stdbuf_parse_options(argc, argv, &options, &diag)) {
        if (!options.suppress_try_help) {
            bx_cli_print_try_help(options.progname);
        }
        return 125;
    }
    if (options.show_help) {
        bx_stdbuf_print_help(stdout, options.progname);
        return 0;
    }
    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }
    if (options.first_operand >= argc) {
        bx_diag(&diag, "missing operand");
        bx_cli_print_try_help(options.progname);
        return 125;
    }
    if (!bx_stdbuf_has_selected_mode(&options)) {
        bx_diag(&diag, "you must specify a buffering mode option");
        bx_cli_print_try_help(options.progname);
        return 125;
    }
    if (!bx_stdbuf_set_stream_environment(&options, &diag)) {
        return 125;
    }

    char *module = bx_preload_find_runtime_module(
        "libstdbuf.so",
        BX_STDBUF_INSTALL_SUBDIR,
        BX_STDBUF_INSTALLED_PATH
    );
    if (module == NULL) {
        bx_diag(&diag, "failed to find 'libstdbuf.so'");
        return 125;
    }
    bool preload_set = bx_stdbuf_set_preload_environment(module, &diag);
    free(module);
    if (!preload_set) {
        return 125;
    }

    char **command_argv = argv + options.first_operand;
    int exec_error = bx_child_exec_argv(command_argv);
    bx_diag(&diag, "failed to run command '%s': %s", command_argv[0], strerror(exec_error));
    return exec_error == ENOENT ? 127 : 126;
}
