#ifndef BX_COMMON_CLI_COMMON_H
#define BX_COMMON_CLI_COMMON_H

#include <stdbool.h>
#include <stdio.h>

#include "bx/diag.h"

const char* bx_cli_progname(const char* argv0, const char* fallback);
void bx_cli_print_version(const char* progname);
void bx_cli_print_try_help(const char* progname);

typedef void (*bx_cli_help_fn)(FILE* stream, const char* progname);

int bx_cli_maybe_handle_help_or_version(
    int argc,
    char** argv,
    const char* fallback,
    const char* short_help_opt,
    const char* short_version_opt,
    bx_cli_help_fn print_help
);

void bx_cli_diag_option_requires_arg(
    struct bx_diag_ctx* diag,
    int optopt,
    int optind,
    int argc,
    char* const argv[]
);
void bx_cli_diag_unrecognized_option(
    struct bx_diag_ctx* diag,
    int optopt,
    int optind,
    int argc,
    char* const argv[]
);
void bx_cli_diag_missing_operand(struct bx_diag_ctx* diag);
void bx_cli_diag_missing_operand_after(struct bx_diag_ctx* diag, const char* operand);
void bx_cli_diag_extra_operand(struct bx_diag_ctx* diag, const char* operand);

bool bx_cli_emit_delimited(const char* value, int delimiter, struct bx_diag_ctx* diag);
bool bx_cli_emit_line(const char* value, bool zero_terminated, struct bx_diag_ctx* diag);
bool bx_cli_flush_stdout(struct bx_diag_ctx* diag);

#endif /* BX_COMMON_CLI_COMMON_H */
