#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lib/cli_common.h"
#include "option_scan.h"
#include "path_ops.h"

const char* bx_cli_progname(const char* argv0, const char* fallback) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return fallback;
    }

    const char* base = bx_path_basename_ptr(argv0);
    if (base[0] != '\0') {
        return base;
    }

    return argv0;
}

void bx_cli_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

int bx_cli_scan_help_version_prefix(
    int argc, char** argv, int* first_operand, struct bx_diag_ctx* diag,
    bx_cli_help_fn print_help
) {
    enum { OPT_HELP, OPT_VERSION };
    static const struct bx_option_spec specs[] = {
        {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
        {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    };
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_PREFIX | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option = bx_option_next(&cursor);
    if (option.event == BX_OPTION_END) {
        *first_operand = (int)option.index;
        return -1;
    }
    if (option.event == BX_OPTION_MATCH) {
        if (option.spec->id == OPT_HELP)
            print_help(stdout, diag->progname);
        else
            bx_cli_print_version(diag->progname);
        return 0;
    }

    if (option.short_name)
        bx_diag(diag, "invalid option -- '%c'", option.short_name);
    else
        bx_diag(diag, "unrecognized option '%s'", option.token);
    bx_cli_print_try_help(diag->progname);
    return diag->exit_status;
}

int bx_cli_maybe_handle_help_or_version(
    int argc,
    char** argv,
    const char* fallback,
    const char* short_help_opt,
    const char* short_version_opt,
    bx_cli_help_fn print_help
) {
    const char* progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, fallback);

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];

        if (arg == NULL) {
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            break;
        }

        if (strcmp(arg, "--help") == 0 || (short_help_opt != NULL && strcmp(arg, short_help_opt) == 0)) {
            print_help(stdout, progname);
            return 0;
        }

        if (strcmp(arg, "--version") == 0
            || (short_version_opt != NULL && strcmp(arg, short_version_opt) == 0)) {
            bx_cli_print_version(progname);
            return 0;
        }
    }

    return -1;
}

void bx_cli_print_try_help(const char* progname) {
    fprintf(stderr, "Try '%s --help' for more information.\n", progname);
}

void bx_cli_diag_missing_operand(struct bx_diag_ctx* diag) {
    bx_diag(diag, "missing operand");
}

void bx_cli_diag_missing_operand_after(struct bx_diag_ctx* diag, const char* operand) {
    bx_diag(diag, "missing operand after '%s'", operand);
}

void bx_cli_diag_extra_operand(struct bx_diag_ctx* diag, const char* operand) {
    bx_diag(diag, "extra operand '%s'", operand);
}

bool bx_cli_emit_delimited(const char* value, int delimiter, struct bx_diag_ctx* diag) {
    if (fputs(value, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    if (fputc(delimiter, stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}

bool bx_cli_emit_line(const char* value, bool zero_terminated, struct bx_diag_ctx* diag) {
    return bx_cli_emit_delimited(value, zero_terminated ? '\0' : '\n', diag);
}

bool bx_cli_flush_stdout(struct bx_diag_ctx* diag) {
    if (fflush(stdout) == EOF) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    return true;
}
