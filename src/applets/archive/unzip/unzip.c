#define _GNU_SOURCE

#include <stdio.h>

#include "cli_common.h"
#include "ctx.h"
#include "ops.h"
#include "ziputils.h"
#include "dispatch/applets.h"
#include "unzip_parse.h"

int bx_unzip_main(int argc, char** argv) {
    const char* tool_name = "unzip";

    // Initialize terminal behavior and color support used by the CLI helpers
    zu_cli_init_terminal();

    // Create a context object that owns all parsed state and transient buffers
    // Execution and cleanup are centralized around this object
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        zu_cli_error(tool_name, "failed to allocate context");
        return ZU_STATUS_OOM;
    }

    // Parse CLI arguments into ctx and translate parser failures into exit codes below
    int parse_rc = zu_unzip_parse_args(argc, argv, ctx);

    // Usage requests are treated as a normal flow that prints help and exits
    if (parse_rc == ZU_STATUS_USAGE) {
        zu_context_free(ctx);
        return zu_cli_map_unzip_exit(parse_rc);
    }

    // Any other parse error is treated as failure and surfaced via a status string where possible
    if (parse_rc != ZU_STATUS_OK) {
        zu_cli_error(tool_name, "argument parsing failed: %s", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return zu_cli_map_unzip_exit(parse_rc);
    }

    // Version-only mode is triggered by -v in zipinfo mode with no archive argument
    if (!ctx->archive_path && ctx->zipinfo_mode && ctx->verbose) {
        zu_unzip_print_version(stdout);
        zu_context_free(ctx);
        return 0;
    }

    /*
     * Dry-run normalization
     * - If the user asked for dry-run without list/test, convert to list-only
     * - Force verbose output so the user sees intended operations
     */
    if (ctx->dry_run && !ctx->list_only && !ctx->test_integrity) {
        ctx->list_only = true;
    }
    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    // Finalize tool_name used for messaging and compatibility warnings
    tool_name = ctx->zipinfo_mode ? "zipinfo" : "unzip";

    // Warn about known gaps, then print traces of effective options for debugging
    // emit_unzip_stub_warnings(ctx, tool_name);
    zu_unzip_trace_effective_defaults(ctx);
    zu_cli_emit_option_trace(tool_name, ctx);

    // Execute the operation configured in ctx
    // All filesystem and archive interactions are handled by the execution layer
    int exec_rc = zu_unzip_run(ctx);

    // Surface any contextual error message produced by the execution layer
    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        zu_cli_error(tool_name, "%s", ctx->error_msg);
    }

    zu_context_free(ctx);
    return zu_cli_map_unzip_exit(exec_rc);
}
