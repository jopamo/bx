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
