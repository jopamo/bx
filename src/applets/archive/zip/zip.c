#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_common.h"
#include "ctx.h"
#include "ops.h"
#include "ziputils.h"
#include "dispatch/applets.h"
#include "zip_parse.h"
#include "zipnote.h"

/*
 * Map the numeric compression method to a human-readable name
 *
 * Method IDs follow the ZIP format conventions used by the rest of the codebase
 * - 0: stored
 * - 8: deflated
 * - 12: bzip2
 */
static const char* compression_method_name(int method) {
    switch (method) {
        case 0:
            return "store";
        case 12:
            return "bzip2";
        case 8:
        default:
            return "deflate";
    }
}

/*
 * Trace the final derived configuration after argument parsing
 *
 * This is intended for debugging and tests
 * - Shows high-impact toggles that change I/O behavior and archive semantics
 * - Avoids dumping every single flag, zu_cli_emit_option_trace handles detailed traces
 */
static void trace_effective_zip_defaults(ZContext* ctx) {
    zu_trace_option(ctx, "effective compression: %s level %d", compression_method_name(ctx->compression_method), ctx->compression_level);

    zu_trace_option(ctx, "paths: %s (recursive %s)", ctx->store_paths ? "preserve" : "junk", ctx->recursive ? "on" : "off");

    const char* target = ctx->output_to_stdout ? "stdout" : (ctx->output_path ? ctx->output_path : (ctx->archive_path ? ctx->archive_path : "(unset)"));
    zu_trace_option(ctx, "output target: %s", target);

    const char* mode = ctx->difference_mode ? "delete" : (ctx->freshen ? "freshen" : (ctx->update ? "update" : (ctx->filesync ? "filesync" : "create/modify")));
    zu_trace_option(ctx, "mode: %s%s%s%s", mode, ctx->remove_source ? " +move" : "", ctx->encrypt ? " +encrypt" : "", ctx->dry_run ? " +dry-run" : "");

    zu_trace_option(ctx, "quiet level: %d, verbose: %s", ctx->quiet_level, ctx->verbose ? "on" : "off");
}

int bx_zip_main(int argc, char** argv) {
    const char* tool_name = "zip";

    // Initialize terminal output handling for consistent colors and formatting
    zu_cli_init_terminal();

    // Context owns all parsed flags, strings, and execution state
    ZContext* ctx = zu_context_create();
    if (!ctx) {
        zu_cli_error(tool_name, "failed to allocate context");
        return ZU_STATUS_OOM;
    }

    // This front-end is for archive creation/modification paths
    ctx->modify_archive = true;

    // Determine which behavior to enable based on argv0
    bool invoked_as_zipcloak = zu_cli_name_matches(argv[0], "zipcloak");
    bool is_zipnote = zu_cli_name_matches(argv[0], "zipnote");

    // zipcloak exists in Info-ZIP for encryption, which is not supported in this build
    if (invoked_as_zipcloak) {
        zu_cli_error(tool_name, "zipcloak/encryption is not supported in this build");
        zu_context_free(ctx);
        return zu_cli_map_zip_exit(ZU_STATUS_NOT_IMPLEMENTED);
    }
    else if (is_zipnote) {
        tool_name = "zipnote";
    }

    if (is_zipnote)
        ctx->zipnote_mode = true;

    // Parse CLI options into ctx, then normalize output behavior for dry-run
    int parse_rc = zu_zip_parse_args(argc, argv, ctx, is_zipnote);
    if (parse_rc != ZU_STATUS_OK) {
        if (parse_rc != ZU_STATUS_USAGE)
            zu_cli_error(tool_name, "argument parsing failed: %s", zu_status_str(parse_rc));
        zu_context_free(ctx);
        return zu_cli_map_zip_exit(parse_rc);
    }

    // Version-only mode
    if (ctx->version_only) {
        zu_zip_print_version(stdout);
        zu_context_free(ctx);
        return 0;
    }

    if (ctx->dry_run) {
        ctx->quiet = false;
        ctx->verbose = true;
    }

    trace_effective_zip_defaults(ctx);
    zu_cli_emit_option_trace(tool_name, ctx);

    // zipnote uses stdin for edit streams, so -z is rejected there to avoid ambiguity
    if (is_zipnote && ctx->zip_comment_specified) {
        zu_cli_error(tool_name, "zipnote: -z is not supported (use zip -z instead)");
        zu_context_free(ctx);
        return zu_cli_map_zip_exit(ZU_STATUS_USAGE);
    }

    /*
     * If -z was specified, stdin becomes the archive-comment stream
     * This is incompatible with reading file data from stdin ("-" file operand)
     */
    if (ctx->zip_comment_specified) {
        for (size_t i = 0; i < ctx->include.len; ++i) {
            if (strcmp(ctx->include.items[i], "-") == 0) {
                zu_cli_error(tool_name, "-z cannot be used when reading file data from stdin");
                zu_context_free(ctx);
                return ZU_STATUS_USAGE;
            }
        }

        int zrc = zu_zip_read_comment(ctx);
        if (zrc != ZU_STATUS_OK) {
            zu_cli_error(tool_name, "failed to read archive comment: %s", zu_status_str(zrc));
            zu_context_free(ctx);
            return zu_cli_map_zip_exit(zrc);
        }
    }

    /*
     * Optional log file
     * - Opened after parsing so -la and -lf are resolved first
     * - Binary mode so byte-accurate logs remain stable across environments
     */
    if (ctx->log_path) {
        if (zu_cli_open_log(ctx) != ZU_STATUS_OK) {
            zu_cli_error(tool_name, "could not open log file '%s'", ctx->log_path);
            zu_context_free(ctx);
            return ZU_STATUS_IO;
        }
    }

    // Dispatch to zipnote or zip execution path
    int exec_rc;
    if (is_zipnote) {
        exec_rc = ctx->zipnote_write ? zu_zipnote_apply(ctx, tool_name) : zu_zipnote_list(ctx);
    }
    else {
        exec_rc = zu_zip_run(ctx);
    }

    // Execution layer may set a human-readable error string for the final failure
    if (exec_rc != ZU_STATUS_OK && ctx->error_msg[0] != '\0') {
        zu_cli_error(tool_name, "%s", ctx->error_msg);
    }

    zu_context_free(ctx);
    return zu_cli_map_zip_exit(exec_rc);
}
