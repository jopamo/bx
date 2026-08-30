#define _POSIX_C_SOURCE 200809L

#include "cli_common.h"

#include <stdarg.h>
#include <string.h>

#include "lib/color.h"
#include "lib/path_ops.h"

/*
 * CLI formatting and logging helpers shared by zip/unzip front-ends
 *
 * Responsibilities
 * - Detect whether stdout is a TTY and enable ANSI color output accordingly
 * - Provide consistent color palette strings through ZuCliColors
 * - Implement uniform error and warning emission with tool-prefixed labels
 * - Provide helpers for usage formatting (sections and option rows)
 * - Emit an option-resolution trace collected during parsing/execution
 *
 * Design constraints
 * - Color decision is based on stdout TTY detection so piping disables colors by default
 * - All formatting helpers remain small and non-allocating to keep error paths simple
 * - Output is line-oriented to support grep and log collection
 */

/*
 * Whether ANSI color output is enabled for this process
 * - Set during zu_cli_init_terminal
 * - Consulted by zu_cli_colors
 */
/*
 * Initialize terminal-related behavior for this process
 *
 * Current policy
 * - Enable colors only when stdout is a TTY
 *   This keeps piped output clean and avoids writing escape sequences into files
 */
void zu_cli_init_terminal(void) {
    (void)bx_color_enabled();
}

/*
 * Return a pointer to the active color palette
 * - The returned pointer is stable for the lifetime of the process
 * - Callers should not cache the pointer across zu_cli_init_terminal in other programs
 */
const ZuCliColors* zu_cli_colors(void) {
    static ZuCliColors colors;
    colors.reset = bx_color_reset();
    colors.bold = bx_color_bold();
    colors.red = bx_color_red();
    colors.green = bx_color_green();
    colors.yellow = bx_color_yellow();
    colors.cyan = bx_color_cyan();
    return &colors;
}

/*
 * Check whether argv0 refers to a specific tool name
 *
 * Behavior
 * - Extracts basename from argv0
 * - Compares exact match against name
 *
 * Notes
 * - On Linux-only builds, '/' is sufficient, but the conditional block keeps behavior tolerant
 *   if argv0 contains backslashes from mixed environments or wrappers
 */
bool zu_cli_name_matches(const char* argv0, const char* name) {
    if (!argv0 || !name)
        return false;

    return strcmp(bx_path_basename_ptr(argv0), name) == 0;
}

/*
 * Shared implementation for error/warning style messages
 *
 * Format
 * - "<color><tool> <label>:<reset> <message>\n"
 *
 * Parameters
 * - to: output stream, typically stderr
 * - tool: tool name prefix shown to the user
 * - label: "error" or "warning"
 * - color: ANSI color sequence or empty string depending on palette selection
 * - fmt/args: printf-style format string and arguments
 */
static void vmessage(FILE* to, const char* tool, const char* label, const char* color, const char* fmt, va_list args) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%s%s %s:%s ", color, tool, label, c->reset);
    vfprintf(to, fmt, args);
    fprintf(to, "\n");
}

/*
 * Print an error message to stderr
 * - Uses red label styling when colors are enabled
 * - Does not terminate the process, callers decide control flow
 */
void zu_cli_error(const char* tool, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vmessage(stderr, tool, "error", zu_cli_colors()->red, fmt, args);
    va_end(args);
}

/*
 * Print a warning message to stderr
 * - Uses yellow label styling when colors are enabled
 * - Intended for compatibility gaps and non-fatal conditions
 */
void zu_cli_warn(const char* tool, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vmessage(stderr, tool, "warning", zu_cli_colors()->yellow, fmt, args);
    va_end(args);
}

/*
 * Print one usage row describing an option
 * - flags is the left column, padded for alignment
 * - desc is the right column description
 */
void zu_cli_print_opt(FILE* to, const char* flags, const char* desc) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "  %s%-24s%s %s\n", c->green, flags, c->reset, desc);
}

/*
 * Print a usage section header
 * - Used to visually group related options
 */
void zu_cli_print_section(FILE* to, const char* title) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "\n%s%s:%s\n", c->cyan, title, c->reset);
}

/*
 * Emit an option-resolution trace collected during parsing and normalization
 *
 * This is gated by verbosity switches so normal runs remain quiet
 * - verbose enables trace for interactive diagnostics
 * - log_info enables trace when running with structured logging enabled
 * - dry_run enables trace because users expect to see what would happen
 *
 * Output sink
 * - Uses zu_log(ctx, ...) so the trace can be routed to stderr or a log file depending on ctx
 */
void zu_cli_emit_option_trace(const char* tool, ZContext* ctx) {
    if (!(ctx->verbose || ctx->log_info || ctx->dry_run))
        return;

    if (ctx->option_events.len == 0)
        return;

    zu_log(ctx, "%s option resolution:\n", tool);
    for (size_t i = 0; i < ctx->option_events.len; ++i) {
        zu_log(ctx, "  %s\n", ctx->option_events.items[i]);
    }
}

int zu_cli_map_zip_exit(int status) {
    switch (status) {
        case ZU_STATUS_OK:
            return 0;
        case ZU_STATUS_USAGE:
            return 16;
        case ZU_STATUS_IO:
            return 2;
        case ZU_STATUS_OOM:
            return 5;
        case ZU_STATUS_NO_FILES:
            return 12;
        case ZU_STATUS_NOT_IMPLEMENTED:
        default:
            return 3;
    }
}

int zu_cli_map_unzip_exit(int status) {
    switch (status) {
        case ZU_STATUS_OK:
            return 0;
        case ZU_STATUS_USAGE:
            return 10;
        case ZU_STATUS_NO_FILES:
            return 11;
        case ZU_STATUS_IO:
            return 2;
        case ZU_STATUS_OOM:
            return 5;
        case ZU_STATUS_NOT_IMPLEMENTED:
        default:
            return 3;
    }
}

int zu_cli_open_log(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;
    if (!ctx->log_path)
        return ZU_STATUS_OK;

    ctx->log_file = fopen(
        ctx->log_path, ctx->log_append ? "ab" : "wb");
    return ctx->log_file ? ZU_STATUS_OK : ZU_STATUS_IO;
}
