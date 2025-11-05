#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "lib/cli_common.h"

const char* bx_cli_progname(const char* argv0, const char* fallback) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return fallback;
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
}

void bx_cli_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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

void bx_cli_diag_option_requires_arg(struct bx_diag_ctx* diag, int optopt, int optind, int argc, char* const argv[]) {
    if (optopt != 0) {
        bx_diag(diag, "option requires an argument -- '%c'", optopt);
    }
    else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
        bx_diag(diag, "option requires an argument -- '%s'", argv[optind - 1]);
    }
    else {
        bx_diag(diag, "option requires an argument");
    }
}

void bx_cli_diag_unrecognized_option(struct bx_diag_ctx* diag, int optopt, int optind, int argc, char* const argv[]) {
    if (optopt != 0) {
        bx_diag(diag, "invalid option -- '%c'", optopt);
    }
    else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
        bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
    }
    else {
        bx_diag(diag, "unrecognized option");
    }
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
