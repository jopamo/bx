#define _GNU_SOURCE

#include "unzip_parse.h"

#include <ctype.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli_common.h"
#include "ziputils.h"
#include "lib/args_common.h"

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
 * Convert internal status codes into conventional process exit codes
 * - Keep this mapping stable for scripts
 * - Preserve a distinct code for common error classes where feasible
 */
/*
 * Centralized compatibility warnings
 * - Keeps option parsing focused on state changes rather than messaging
 * - Groups related warnings so users see a minimal set of actionable notices
 */
/*
static void warn_unzip_stub(ZContext* ctx, const char* tool, const char* opt, const char* detail) {
    char buf[256];
    const char* msg = detail ? detail : "behavior incomplete";
    snprintf(buf, sizeof(buf), "%s: %s is stubbed (Info-ZIP parity pending): %s", tool, opt, msg);
    zu_warn_once(ctx, buf);
}

static void emit_unzip_stub_warnings(ZContext* ctx, const char* tool_name) {
    bool used_zipinfo_formatting = ctx->zipinfo_stub_used || (ctx->zipinfo_mode && (ctx->zi_format_specified || ctx->zi_header_explicit || ctx->zi_footer_explicit));

    if (used_zipinfo_formatting) {
        warn_unzip_stub(ctx, tool_name, "zipinfo formatting flags", "output layout and timestamps may differ from Info-ZIP zipinfo/unzip");
    }

    if (ctx->used_long_option) {
        warn_unzip_stub(ctx, tool_name, "long options", "some aliases and negations are not implemented");
    }
}
*/

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
    zu_cli_print_opt(to, "-2", "List filenames only (allow headers)");
    zu_cli_print_opt(to, "-s", "Short listing (default)");
    zu_cli_print_opt(to, "-m", "Medium listing");
    zu_cli_print_opt(to, "-h", "Force header line");
    zu_cli_print_opt(to, "-T", "Print decimal timestamps");

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
 * - getopt_long loop to translate flags into ctx fields
 * - Consume positional args: archive path and optional include patterns
 * - Apply zipinfo post-processing rules that depend on the final option set
 *
 * Return value
 * - ZU_STATUS_OK on success
 * - ZU_STATUS_USAGE when help or invalid usage should be reported
 * - Other ZU_STATUS_* codes for allocation failures or unsupported behaviors
 */
int zu_unzip_parse_args(int argc, char** argv, ZContext* ctx) {
    bx_args_getopt_reset();
    // Some installations provide zipinfo as a hardlink or alias to unzip
    // If argv0 matches zipinfo-like names, default into zipinfo listing mode
    if (zu_cli_name_matches(argv[0], "zipinfo") || zu_cli_name_matches(argv[0], "ii")) {
        ctx->zipinfo_mode = true;
        ctx->list_only = true;
        zu_trace_option(ctx, "zipinfo mode enabled via argv0");
    }

    // Supported long options are intentionally limited to what this implementation covers today
    static const struct option long_opts[] = {
        {"help", no_argument, NULL, '?'},
        {"list", no_argument, NULL, 'l'},
        {"pipe", no_argument, NULL, 'p'},
        {"test", no_argument, NULL, 't'},
        {"password", required_argument, NULL, 'P'},
        {"dry-run", no_argument, NULL, OPT_DRY_RUN},
        {NULL, 0, NULL, 0},
    };

    int opt;
    int positional_seen = 0;
    // Leading '-' in optstring makes getopt_long return 1 for non-option arguments
    while ((opt = bx_args_getopt_long(argc, argv, "-lptd:ovnqvjLi:Xx:hCZ12smMvTz?P:D", long_opts, NULL)) != -1) {
        // Record whether any long options were used so we can emit a targeted warning
        // getopt_long does not provide a direct flag for this, so we inspect argv at optind-1
        if (optind > 0 && optind <= argc && strncmp(argv[optind - 1], "--", 2) == 0) {
            ctx->used_long_option = true;
        }

        switch (opt) {
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
                ctx->target_dir = optarg;
                zu_trace_option(ctx, "-d %s", optarg);
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
                if (zu_strlist_push(&ctx->include, optarg) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-i pattern %s", optarg);
                break;

            case 'x':
                // Add an exclude pattern, removing matches from the final selection
                // Multiple -x flags accumulate in the exclude list
                if (zu_strlist_push(&ctx->exclude, optarg) != 0)
                    return ZU_STATUS_OOM;
                zu_trace_option(ctx, "-x pattern %s", optarg);
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
                // Marked as stubbed because formatting parity is pending
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
        
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;

                ctx->zi_header = false;
                ctx->zi_footer = false;
                ctx->zi_header_explicit = true;
                ctx->zi_footer_explicit = true;

                ctx->zi_list_entries = true;
                ctx->zipinfo_stub_used = true;

                zu_trace_option(ctx, "-1 names only (quiet)");
                break;

            case '2':
                // Zipinfo-style names-only mode that may still allow headers depending on other flags
                // Marked as stubbed because formatting parity is pending
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
        
                ctx->zi_format = ZU_ZI_FMT_NAMES;
                ctx->zi_format_specified = true;

                ctx->zi_list_entries = true;
                ctx->zipinfo_stub_used = true;

                zu_trace_option(ctx, "-2 names only");
                break;

            case 's':
                // Zipinfo short listing selector
                // Marked stubbed until formatting and timestamps align with Info-ZIP
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
        
                ctx->zi_format = ZU_ZI_FMT_SHORT;
                ctx->zi_format_specified = true;

                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-s short listing");
                break;

            case 'm':
                // Zipinfo medium listing selector
                // Marked stubbed until formatting and timestamps align with Info-ZIP
                ctx->zipinfo_mode = true;
                ctx->list_only = true;
        
                ctx->zi_format = ZU_ZI_FMT_MEDIUM;
                ctx->zi_format_specified = true;

                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-m medium listing");
                break;

            case 'h':
                // In zipinfo mode, -h forces printing a header line
                // Outside zipinfo mode, this implementation treats -h as usage for convenience
                if (ctx->zipinfo_mode) {
                    ctx->zi_header = true;
                    ctx->zi_header_explicit = true;
                    ctx->list_only = true;
            
                    ctx->zipinfo_stub_used = true;
                    zu_trace_option(ctx, "-h show header");
                }
                else {
                    print_usage(stdout, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                break;

            case 'M':
                // Zipinfo pager flag
                // Currently a no-op placeholder, kept for CLI compatibility
                ctx->zipinfo_mode = true;
                ctx->zi_allow_pager = true;
                ctx->list_only = true;
        
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-M pager (noop)");
                break;

            case 'T':
                // Zipinfo decimal timestamp output selector
                // Marked stubbed until timestamp formatting matches Info-ZIP precisely
                ctx->zipinfo_mode = true;
                ctx->zi_decimal_time = true;
                ctx->list_only = true;
        
                ctx->zipinfo_stub_used = true;
                zu_trace_option(ctx, "-T decimal time");
                break;

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
                if (optind > 0 && optind <= argc && strcmp(argv[optind - 1], "-") == 0) {
                    if (positional_seen == 0) {
                        ctx->archive_path = "-";
                        zu_trace_option(ctx, "archive path set to -");
                    }
                    else {
                        if (zu_strlist_push(&ctx->include, "-") != 0)
                            return ZU_STATUS_OOM;
                        zu_trace_option(ctx, "include pattern -");
                    }
                    positional_seen++;
                    break;
                }

                // getopt_long maps --help and unknown/invalid flags to '?'
                print_usage(stdout, argv[0]);
                return ZU_STATUS_USAGE;

            case 1:
                // Non-option argument (due to leading '-' in optstring)
                if (positional_seen == 0) {
                    ctx->archive_path = optarg;
                    zu_trace_option(ctx, "archive path set to %s", optarg);
                }
                else {
                    if (zu_strlist_push(&ctx->include, optarg) != 0)
                        return ZU_STATUS_OOM;
                    zu_trace_option(ctx, "include pattern %s", optarg);
                }
                positional_seen++;
                break;

            default:
                // Defensive default for unexpected getopt return values
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
        }
    }

    // "-" as archive path (stdin) is allowed and passed to execution layer

    // Support for reading the archive from stdin is intentionally incomplete here

    // If archive_path not already set via non‑option arguments, process traditional positionals
    if (ctx->archive_path == NULL) {
        /*
         * Positional arguments
         * - First positional is archive path
         * - Remaining positionals are treated as include patterns (Info-ZIP style)
         */
        if (optind >= argc) {
            // "unzip -v" or "zipinfo -v" with no args prints version in this implementation
            if (ctx->zipinfo_mode && ctx->verbose) {
                ctx->archive_path = NULL;
                return ZU_STATUS_OK;
            }
            print_usage(stderr, argv[0]);
            return ZU_STATUS_USAGE;
        }

        ctx->archive_path = argv[optind++];

        zu_trace_option(ctx, "archive path set to %s", ctx->archive_path);

        // Any remaining args after the archive are treated as patterns to include
        for (int i = optind; i < argc; ++i) {
            if (zu_strlist_push(&ctx->include, argv[i]) != 0)
                return ZU_STATUS_OOM;
            zu_trace_option(ctx, "include pattern %s", argv[i]);
        }
    }
    else {
        // archive_path already set via non-option argument; add any remaining arguments as patterns
        for (int i = optind; i < argc; ++i) {
            if (zu_strlist_push(&ctx->include, argv[i]) != 0)
                return ZU_STATUS_OOM;
            zu_trace_option(ctx, "include pattern %s", argv[i]);
        }
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

    return ZU_STATUS_OK;
}
