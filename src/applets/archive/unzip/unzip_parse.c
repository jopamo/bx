#define _GNU_SOURCE

#include "unzip_parse.h"

#include <stdbool.h>
#include <stdio.h>

#include "cli_common.h"
#include "ziputils.h"
#include "lib/option_scan.h"

enum { OPT_DRY_RUN = 1000 };

/*
 * Tool name used in diagnostics
 * - Defaults to "unzip"
 * - Can switch to "zipinfo" when invoked via argv0 or zipinfo-related flags
 */
static const char* unzip_parse_tool(const ZContext* ctx) {
    return ctx->zipinfo_mode ? "zipinfo" : "unzip";
}

/*
 * Trace the final effective configuration after parsing
 * - Intended for debugging and regression testing
 * - Shows the derived mode and the high-impact toggles that affect behavior
 */
void zu_unzip_trace_effective_defaults(ZContext* ctx) {
    const char* mode = ctx->list_only ? "list" : (ctx->test_integrity ? "test" : "extract");
    zu_trace_option(ctx, "mode: %s%s", mode, ctx->dry_run ? " +dry-run" : "");

    const char* target = ctx->target_dir ? ctx->target_dir : "(cwd)";
    zu_trace_option(ctx, "target dir: %s", target);

    const char* ov = "prompt";
    if (ctx->overwrite_policy == ZU_OVERWRITE_ALWAYS)
        ov = "always";
    else if (ctx->overwrite_policy == ZU_OVERWRITE_NEVER)
        ov = "never";
    zu_trace_option(ctx, "overwrite: %s", ov);

    zu_trace_option(ctx, "pattern match: include=%zu exclude=%zu case %s", ctx->include.len, ctx->exclude.len, ctx->match_case ? "sensitive" : "insensitive");

    zu_trace_option(ctx, "zipinfo mode: %s (format %d)", ctx->zipinfo_mode ? "on" : "off", ctx->zi_format);
}

/*
 * Version output
 * - Mirrored after typical Info-ZIP "unzip -v" style behavior in this codebase
 */
void zu_unzip_print_version(FILE* to) {
    fprintf(to, "UnZip 6.00 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n");
}

/*
 * Usage output
 * - Kept human-readable and grouped by feature area
 * - Long options are documented where implemented
 */
static void print_usage(FILE* to, const char* argv0) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%sUsage:%s %s%s [options] archive.zip [patterns...]%s\n", c->bold, c->reset, c->green, argv0, c->reset);

    fprintf(to, "\nInfo-ZIP compliant extraction utility.\n");

    zu_cli_print_section(to, "Common Options");
    zu_cli_print_opt(to, "-l", "List contents only");
    zu_cli_print_opt(to, "-t", "Test archive integrity");
    zu_cli_print_opt(to, "-p", "Extract files to pipe (stdout)");
    zu_cli_print_opt(to, "-d <dir>", "Extract into specified directory");
    zu_cli_print_opt(to, "-o / -n", "Overwrite / Never overwrite existing files");
    zu_cli_print_opt(to, "-q / -qq", "Quiet mode (stackable)");
    zu_cli_print_opt(to, "-v", "Verbose output (or print version)");
    zu_cli_print_opt(to, "-D", "Do not restore directory timestamps");
    zu_cli_print_opt(to, "-DD", "Do not restore any timestamps");

    zu_cli_print_section(to, "Selection & Modifiers");
    zu_cli_print_opt(to, "-x <pat>", "Exclude files matching pattern");
    zu_cli_print_opt(to, "-i <pat>", "Include only files matching pattern");
    zu_cli_print_opt(to, "-C", "Case-insensitive pattern matching");
    zu_cli_print_opt(to, "-j", "Junk paths (flatten directories)");
    zu_cli_print_opt(to, "-L", "Convert filenames to lowercase (stub)");
    zu_cli_print_opt(to, "-X", "Restore UID/GID info (stub)");

    zu_cli_print_section(to, "Zipinfo Mode (-Z)");
    zu_cli_print_opt(to, "-1", "List filenames only (one per line)");

    zu_cli_print_section(to, "Diagnostics");
    zu_cli_print_opt(to, "--dry-run", "Show operations without writing");
    zu_cli_print_opt(to, "--help", "Show this help");

    fprintf(to, "\n");
}

/*
 * Parse CLI arguments and populate ZContext
 *
 * Parsing phases
 * - Pre-scan argv0 to enable zipinfo compatibility modes early
 * - Scan options and operands in their original order
 * - Consume positional args: archive path and optional include patterns
 * - Apply zipinfo post-processing rules that depend on the final option set
 *
 * Return value
 * - ZU_STATUS_OK on success
 * - ZU_STATUS_USAGE when help or invalid usage should be reported
 * - Other ZU_STATUS_* codes for allocation failures or unsupported behaviors
 */
int zu_unzip_parse_args(int argc, char** argv, ZContext* ctx) {
    // Some installations provide zipinfo as a hardlink or alias to unzip
    // If argv0 matches zipinfo-like names, default into zipinfo listing mode
    if (zu_cli_name_matches(argv[0], "zipinfo") || zu_cli_name_matches(argv[0], "ii")) {
        ctx->zipinfo_mode = true;
        ctx->list_only = true;
        zu_trace_option(ctx, "zipinfo mode enabled via argv0");
    }

    static const struct bx_option_spec specs[] = {
        {'l', 'l', "list", BX_OPTION_NO_VALUE},
        {'p', 'p', "pipe", BX_OPTION_NO_VALUE},
        {'t', 't', "test", BX_OPTION_NO_VALUE},
        {'d', 'd', NULL, BX_OPTION_REQUIRED_VALUE},
        {'o', 'o', NULL, BX_OPTION_NO_VALUE},
        {'v', 'v', NULL, BX_OPTION_NO_VALUE},
        {'n', 'n', NULL, BX_OPTION_NO_VALUE},
        {'q', 'q', NULL, BX_OPTION_NO_VALUE},
        {'j', 'j', NULL, BX_OPTION_NO_VALUE},
        {'L', 'L', NULL, BX_OPTION_NO_VALUE},
        {'i', 'i', NULL, BX_OPTION_REQUIRED_VALUE},
        {'X', 'X', NULL, BX_OPTION_NO_VALUE},
        {'x', 'x', NULL, BX_OPTION_REQUIRED_VALUE},
        {'h', 'h', NULL, BX_OPTION_NO_VALUE},
        {'C', 'C', NULL, BX_OPTION_NO_VALUE},
        {'Z', 'Z', NULL, BX_OPTION_NO_VALUE},
        {'1', '1', NULL, BX_OPTION_NO_VALUE},
        {'2', '2', NULL, BX_OPTION_NO_VALUE},
        {'s', 's', NULL, BX_OPTION_NO_VALUE},
        {'m', 'm', NULL, BX_OPTION_NO_VALUE},
        {'M', 'M', NULL, BX_OPTION_NO_VALUE},
        {'T', 'T', NULL, BX_OPTION_NO_VALUE},
        {'z', 'z', NULL, BX_OPTION_NO_VALUE},
        {'?', '?', "help", BX_OPTION_NO_VALUE},
        {'P', 'P', "password", BX_OPTION_REQUIRED_VALUE},
        {'D', 'D', NULL, BX_OPTION_NO_VALUE},
        {OPT_DRY_RUN, 0, "dry-run", BX_OPTION_NO_VALUE},
    };

    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          specs, sizeof(specs) / sizeof(specs[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (ctx->archive_path == NULL) {
                if (option.token[0] == '\0') {
                    zu_cli_error(unzip_parse_tool(ctx), "empty archive path");
                    return ZU_STATUS_USAGE;
                }
                ctx->archive_path = option.token;
                zu_trace_option(ctx, "archive path set to %s", option.token);
            }
            else {
                if (zu_strlist_push(&ctx->include, option.token) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "include pattern %s", option.token);
            }
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            print_usage(stderr, argv[0]);
            return ZU_STATUS_USAGE;
        }
        switch (option.spec->id) {
            case 'l':
                // unzip: list only
                // zipinfo: treat as "long" listing format selector
                if (ctx->zipinfo_mode) {
                    ctx->zi_format = ZU_ZI_FMT_LONG;
                    ctx->zi_format_specified = true;
                    ctx->list_only = true;
                }
                else {
                    ctx->list_only = true;
                }
                zu_trace_option(ctx, "-l list");
                break;

            case 'p':
                // Extract file data to stdout instead of creating files
                // Directory structure and metadata are not applicable in this mode
                ctx->output_to_stdout = true;
                zu_trace_option(ctx, "-p output to stdout");
                break;

            case 'P':
                // Password entry is explicitly unsupported by spec
                zu_cli_error(unzip_parse_tool(ctx), "option -P (password) is not supported");
                return ZU_STATUS_USAGE;

            case 't':
                // unzip: enable integrity testing mode
                // zipinfo: toggle footer printing behavior
                if (ctx->zipinfo_mode) {
                    ctx->zi_footer = true;
                    ctx->zi_footer_explicit = true;
                    ctx->list_only = true;
                }
                else {
                    ctx->test_integrity = true;
                }
                zu_trace_option(ctx, "-t test");
                break;

            case 'd':
                // Set extraction target directory
                // Actual directory creation and path sanitization are handled in execution code
                if (option.value[0] == '\0') {
                    zu_cli_error(unzip_parse_tool(ctx), "empty target directory");
                    return ZU_STATUS_USAGE;
                }
                ctx->target_dir = option.value;
                zu_trace_option(ctx, "-d %s", option.value);
                break;

            case 'o':
                // Always overwrite existing files during extraction
                ctx->overwrite_policy = ZU_OVERWRITE_ALWAYS;
                zu_trace_option(ctx, "-o overwrite always");
                break;

            case 'n':
                // Never overwrite existing files during extraction
                ctx->overwrite_policy = ZU_OVERWRITE_NEVER;
                zu_trace_option(ctx, "-n never overwrite");
                break;

            case 'j':
                // Flatten directory structure and write all files into the target directory
                // This affects path handling and can cause name collisions
                ctx->store_paths = false;
                zu_trace_option(ctx, "-j junk paths");
                break;

            case 'q':
                // Quiet is stackable to reduce output further
                // Quiet implies non-verbose for deterministic logging behavior
                ctx->quiet_level++;
                ctx->quiet = true;
                ctx->verbose = false;
                zu_trace_option(ctx, "-q quiet level %d", ctx->quiet_level);
                break;

            case 'D':
                if (ctx->d_flag < 2)
                    ctx->d_flag++;
                zu_trace_option(ctx, "-D timestamp suppression level %u", ctx->d_flag);
                break;

            case 'L':
                // Lowercasing is explicitly unsupported by spec
                zu_cli_error(unzip_parse_tool(ctx), "option -L (lowercase names) is not supported");
                return ZU_STATUS_USAGE;

            case 'X':
                // UID/GID restoration is explicitly unsupported by spec
                zu_cli_error(unzip_parse_tool(ctx), "option -X (restore UID/GID) is not supported");
                return ZU_STATUS_USAGE;

            case 'v':
                // For compatibility, -v acts like zipinfo verbose listing
                // Also supports "unzip -v" with no archive args to print version in this program
                ctx->verbose = true;
                ctx->list_only = true;
                ctx->zipinfo_mode = true;
        
                ctx->zi_format = ZU_ZI_FMT_VERBOSE;
                ctx->zi_format_specified = true;
                ctx->zi_show_comments = true;

                zu_trace_option(ctx, "-v verbose");
                break;

            case 'C':
                // Case-insensitive matching is explicitly unsupported by spec
                zu_cli_error(unzip_parse_tool(ctx), "option -C (case-insensitive) is not supported");
                return ZU_STATUS_USAGE;

            case 'i':
                // Add an include pattern, restricting matches to this set
                // Multiple -i flags accumulate in the include list
                if (zu_strlist_push(&ctx->include, option.value) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-i pattern %s", option.value);
                break;

            case 'x':
                // Add an exclude pattern, removing matches from the final selection
                // Multiple -x flags accumulate in the exclude list
                if (zu_strlist_push(&ctx->exclude, option.value) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-x pattern %s", option.value);
                break;

            case 'Z':
                // Explicitly force zipinfo behavior
                // Listing mode is implied and tool_name is switched for diagnostics
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
                zu_trace_option(ctx, "-Z zipinfo mode");
                break;

            case '1':
                // Zipinfo-style names-only mode with no header/footer
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
        
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;

                ctx->zi_header = false;
                ctx->zi_footer = false;
                ctx->zi_header_explicit = true;
                ctx->zi_footer_explicit = true;

                ctx->zi_list_entries = true;

                zu_trace_option(ctx, "-1 names only (quiet)");
                break;

            case 'M':
                ctx->zipinfo_mode = true;
                ctx->zi_allow_pager = true;
                ctx->list_only = true;
                zu_trace_option(ctx, "-M pager");
                break;

            case 'h':
                if (!ctx->zipinfo_mode) {
                    print_usage(stdout, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                /* fall through */
            case '2':
            case 's':
            case 'm':
            case 'T':
                zu_cli_error(unzip_parse_tool(ctx), "zipinfo option -%c is not supported", option.spec->id);
                return ZU_STATUS_USAGE;

            case 'z':
                // Show archive comments only
                if (ctx->zipinfo_mode) {
                    // Likely invoked as -Z -z (or zipinfo -z)
                    // Keep listings enabled (default for zipinfo)
                    ctx->zi_show_comments = true;
                }
                else {
                    // Invoked as unzip -z
                    ctx->zipinfo_mode = true;
                    ctx->list_only = true;
                    ctx->zi_show_comments = true;
                    ctx->zi_list_entries = false;
                    ctx->zi_header = false;
                    ctx->zi_footer = false;
                    ctx->zi_header_explicit = true;
                    ctx->zi_footer_explicit = true;
                }

                zu_trace_option(ctx, "-z show comments");
                break;

            case OPT_DRY_RUN:
                // Dry run means do not write files
                // Force verbose output so the user can see what would have happened
                // If not already in list/test, this front-end converts dry-run into listing
                ctx->dry_run = true;
                ctx->verbose = true;
                ctx->quiet = false;
                ctx->list_only = ctx->list_only || ctx->zipinfo_mode;

                zu_trace_option(ctx, "--dry-run");
                break;

            case '?':
                print_usage(stdout, argv[0]);
                return ZU_STATUS_USAGE;
        }
    }

    if (ctx->archive_path == NULL) {
        if (ctx->zipinfo_mode && ctx->verbose)
            return ZU_STATUS_OK;
        print_usage(stderr, argv[0]);
        return ZU_STATUS_USAGE;
    }

    /*
     * Zipinfo post-processing
     * Some zipinfo behaviors depend on whether patterns were provided and which header/footer flags
     * were explicitly set. This block normalizes the final zipinfo state to match expected semantics
     * as closely as this implementation currently supports
     */
    if (ctx->zipinfo_mode) {
        // When patterns are provided, suppress header/footer unless explicitly forced
        if (ctx->include.len > 0) {
            if (!ctx->zi_header_explicit)
                ctx->zi_header = false;
            if (!ctx->zi_footer_explicit)
                ctx->zi_footer = false;
        }

        // If only header/footer flags were set without a format selector, default to non-entry output
        if (!ctx->zi_format_specified && (ctx->zi_header_explicit || ctx->zi_footer_explicit)) {
            ctx->zi_list_entries = false;
        }

        // If footer was forced without a header, disable header to avoid surprising output
        if (!ctx->zi_format_specified && ctx->zi_footer_explicit && !ctx->zi_header_explicit) {
            ctx->zi_header = false;
        }

        // If we are not listing entries and footer was not explicitly requested, suppress footer
        if (!ctx->zi_list_entries && !ctx->zi_footer_explicit) {
            ctx->zi_footer = false;
        }

        ctx->list_only = true;
    }

    if (zu_strlist_normalize_globs(&ctx->include) != 0 ||
        zu_strlist_normalize_globs(&ctx->exclude) != 0)
        return ZU_STATUS_OOM;
    return ZU_STATUS_OK;
}
