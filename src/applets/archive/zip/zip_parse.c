#define _GNU_SOURCE

#include "zip_parse.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "cli_common.h"
#include "ziputils.h"
#include "lib/size_parse.h"
#include "lib/time_parse.h"

static const char* zip_parse_tool(const ZContext* ctx) {
    return ctx->zipnote_mode ? "zipnote" : "zip";
}

static int parse_fast_write_threshold(const char* option, const char* value, ZContext* ctx) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_uint(value, &parsed) || parsed > UINT64_MAX) {
        zu_cli_error(zip_parse_tool(ctx), "invalid value for %s: %s", option, value);
        return ZU_STATUS_USAGE;
    }
    ctx->fast_write_threshold = (uint64_t)parsed;
    return ZU_STATUS_OK;
}

/*
 * Parse a date string used by -t and -tt style filters
 *
 * Supported inputs
 * - YYYY-MM-DD
 * - MMDDYYYY
 *
 * Output
 * - Returns a local time_t via mktime using tm_isdst = -1 so the C library can resolve DST
 * - Returns (time_t)-1 when parsing fails or the token does not consume the full input string
 *
 * Notes
 * - Only basic day/month bounds are checked here, mktime performs additional normalization
 * - This is intentionally strict about trailing characters to avoid surprising filters
 */
static time_t parse_date(const char* str) {
    int year = 0;
    int month = 0;
    int day = 0;
    struct tm tm = {0};
    size_t len = strlen(str);

    if (len == 10u && str[4] == '-' && str[7] == '-'
        && bx_time_parse_fixed_width_int(str, 0, 4, &year)
        && bx_time_parse_fixed_width_int(str, 5, 2, &month)
        && bx_time_parse_fixed_width_int(str, 8, 2, &day)) {
        /* Parsed ISO date. */
    }
    else if (len == 8u
             && bx_time_parse_fixed_width_int(str, 0, 2, &month)
             && bx_time_parse_fixed_width_int(str, 2, 2, &day)
             && bx_time_parse_fixed_width_int(str, 4, 4, &year)) {
        /* Parsed Info-ZIP compact date. */
    }
    else {
        return (time_t)-1;
    }

    if (month < 1 || month > 12 || day < 1 || day > 31)
        return (time_t)-1;
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

/*
 * Read newline-delimited file names from stdin and append them to ctx->include
 *
 * Behavior
 * - Each non-empty line becomes one include entry
 * - Trailing newline is stripped
 * - Empty lines are ignored
 *
 * Error handling
 * - Returns ZU_STATUS_IO if stdin reports a read error
 * - Returns ZU_STATUS_OOM if list growth fails
 */
static int read_stdin_names(ZContext* ctx) {
    char* line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, stdin)) != -1) {
        if (linelen > 0 && line[linelen - 1] == '\n') {
            line[linelen - 1] = '\0';
            linelen--;
        }
        if (linelen > 0) {
            if (zu_strlist_push(&ctx->include, line) != 0) {
                free(line);
                return ZU_STATUS_OOM;
            }
        }
    }

    free(line);

    if (ferror(stdin)) {
        return ZU_STATUS_IO;
    }
    return ZU_STATUS_OK;
}

/*
 * Read an archive comment from stdin into ctx->zip_comment
 *
 * Constraints
 * - ZIP file comment length is limited to UINT16_MAX, enforce that here
 * - The buffer is stored as raw bytes, but is null-terminated for convenience
 *
 * Return values
 * - ZU_STATUS_OK on success
 * - ZU_STATUS_IO on stdin read errors
 * - ZU_STATUS_USAGE if the comment exceeds the ZIP comment length limit
 */
int zu_zip_read_comment(ZContext* ctx) {
    if (!ctx)
        return ZU_STATUS_USAGE;

    size_t cap = 1024;
    size_t len = 0;
    char* buf = malloc(cap);
    if (!buf)
        return ZU_STATUS_OOM;

    size_t got = 0;
    while ((got = fread(buf + len, 1, cap - len, stdin)) > 0) {
        len += got;
        if (len == cap) {
            size_t new_cap = cap * 2;
            char* nb = realloc(buf, new_cap);
            if (!nb) {
                free(buf);
                return ZU_STATUS_OOM;
            }
            buf = nb;
            cap = new_cap;
        }
    }

    if (ferror(stdin)) {
        free(buf);
        return ZU_STATUS_IO;
    }

    if (len > UINT16_MAX) {
        free(buf);
        return ZU_STATUS_USAGE;
    }

    // Ensure a terminating NUL for debug printing and safe string usage
    if (len == cap) {
        char* nb = realloc(buf, cap + 1);
        if (!nb) {
            free(buf);
            return ZU_STATUS_OOM;
        }
        buf = nb;
    }
    buf[len] = '\0';

    free(ctx->zip_comment);
    ctx->zip_comment = buf;
    ctx->zip_comment_len = len;
    return ZU_STATUS_OK;
}

/*
 * Version output
 * - Mirrored after typical Info-ZIP "zip -v" style behavior
 */
void zu_zip_print_version(FILE* to) {
    fprintf(to, "Zip 3.0 (zip-utils rewrite; Info-ZIP compatibility work in progress)\n");
}

/*
 * Print CLI usage
 * - This is the public interface users rely on
 * - Option grouping reflects parsing behavior in parse_zip_args
 */
static void print_usage(FILE* to, const char* argv0) {
    const ZuCliColors* c = zu_cli_colors();
    fprintf(to, "%sUsage:%s %s%s [options] archive.zip [file ...]%s\n", c->bold, c->reset, c->green, argv0, c->reset);

    fprintf(to, "\nInfo-ZIP compliant compression utility (zip-utils).\n");

    zu_cli_print_section(to, "Basic Modes");
    zu_cli_print_opt(to, "(default)", "Create or modify archive");
    zu_cli_print_opt(to, "-f", "Freshen: replace existing entries only");
    zu_cli_print_opt(to, "-u", "Update: replace newer or add new entries");
    zu_cli_print_opt(to, "-d", "Delete patterns from archive");
    zu_cli_print_opt(to, "-m", "Move: delete source files after archiving");
    zu_cli_print_opt(to, "-FS", "Filesync: sync archive with filesystem content");

    zu_cli_print_section(to, "Selection & Filtering");
    zu_cli_print_opt(to, "-r", "Recurse into directories");
    zu_cli_print_opt(to, "-R", "Recurse from current dir (PKZIP style)");
    zu_cli_print_opt(to, "-j", "Junk paths (store basenames only)");
    zu_cli_print_opt(to, "-x, --exclude <pats>", "Exclude patterns");
    zu_cli_print_opt(to, "-i, --include <pats>", "Include patterns");
    zu_cli_print_opt(to, "-@", "Read file names from stdin");
    zu_cli_print_opt(to, "-t <date>", "Include files modified after mmddyyyy");
    zu_cli_print_opt(to, "-tt <date>", "Include files modified before mmddyyyy");

    zu_cli_print_section(to, "Compression & Storage");
    zu_cli_print_opt(to, "-0 ... -9", "Compression level (0=store, 9=best)");
    zu_cli_print_opt(to, "-Z <meth>", "Method: deflate, store, bzip2");
    zu_cli_print_opt(to, "-n <suf>", "Don't compress these suffixes");
    zu_cli_print_opt(to, "-y", "Store symlinks as links (not targets)");
    zu_cli_print_opt(to, "-X", "Strip extra file attributes (UID/GID)");
    zu_cli_print_opt(to, "-D", "Do not create directory entries");

    zu_cli_print_section(to, "Input / Output");
    zu_cli_print_opt(to, "-O <path>", "Write output to different file");
    zu_cli_print_opt(to, "-b <dir>", "Temporary directory");
    zu_cli_print_opt(to, "-o", "Set archive mtime to newest entry");
    zu_cli_print_opt(to, "-", "Use stdout for output or stdin for input");

    zu_cli_print_section(to, "Performance");
    zu_cli_print_opt(to, "--fast-write[=bytes]", "Skip pre-compress size check; optional threshold (default 512KiB)");

    zu_cli_print_section(to, "Text Processing");
    zu_cli_print_opt(to, "-l", "Translate LF to CRLF");
    zu_cli_print_opt(to, "-ll", "Translate CRLF to LF");
    zu_cli_print_opt(to, "-z", "Read archive comment from stdin");

    zu_cli_print_section(to, "Diagnostics");
    zu_cli_print_opt(to, "-q, --quiet", "Quiet mode (stackable: -qq)");
    zu_cli_print_opt(to, "-v, --verbose", "Verbose / Print version info");
    zu_cli_print_opt(to, "-T, --test", "Test archive integrity after write");
    zu_cli_print_opt(to, "--dry-run", "Show what would be done");
    zu_cli_print_opt(to, "-lf <path>", "Log file path");

    fprintf(to, "\n");
}

/* --- Argument Parsing --- */

/*
 * Determine whether a short option character is allowed in a clustered flag token
 *
 * Example
 * -rqv is treated as -r -q -v
 *
 * This list is intentionally restrictive
 * - Options that take arguments must not be part of a cluster
 * - Options that have special multi-letter forms (-FS, -FF, -ll, -la, -li) are handled separately
 */
static bool is_cluster_flag(char c) {
    return (strchr("rjTqvmdfuDXyel", c) != NULL) || (isdigit(c));
}

/*
 * Apply a single clustered short option to the context
 *
 * Return value
 * - ZU_STATUS_OK if applied
 * - ZU_STATUS_NOT_IMPLEMENTED for accepted-but-unsupported behaviors
 * - ZU_STATUS_USAGE for invalid flags
 */
static int apply_cluster_flag(char c, ZContext* ctx) {
    switch (c) {
        case 'r':
            ctx->recursive = true;
            zu_trace_option(ctx, "-r recurse into directories");
            return ZU_STATUS_OK;
        case 'j':
            ctx->store_paths = false;
            zu_trace_option(ctx, "-j junk paths");
            return ZU_STATUS_OK;
        case 'T':
            ctx->test_integrity = true;
            zu_trace_option(ctx, "-T test after write");
            return ZU_STATUS_OK;
        case 'q':
            ctx->quiet_level++;
            ctx->quiet = true;
            ctx->verbose = false;
            zu_trace_option(ctx, "-q quiet level %d", ctx->quiet_level);
            return ZU_STATUS_OK;
        case 'v':
            ctx->verbose = true;
            zu_trace_option(ctx, "-v verbose");
            return ZU_STATUS_OK;
        case 'm':
            ctx->remove_source = true;
            zu_trace_option(ctx, "-m move");
            return ZU_STATUS_OK;
        case 'd':
            ctx->difference_mode = true;
            zu_trace_option(ctx, "-d delete");
            return ZU_STATUS_OK;
        case 'f':
            ctx->freshen = true;
            zu_trace_option(ctx, "-f freshen");
            return ZU_STATUS_OK;
        case 'u':
            ctx->update = true;
            zu_trace_option(ctx, "-u update");
            return ZU_STATUS_OK;
        case 'D':
            ctx->no_dir_entries = true;
            zu_trace_option(ctx, "-D no dir entries");
            return ZU_STATUS_OK;
        case 'X':
            ctx->exclude_extra_attrs = true;
            zu_trace_option(ctx, "-X drop extra attrs");
            return ZU_STATUS_OK;
        case 'y':
            ctx->store_symlinks = true;
            ctx->allow_symlinks = true;
            zu_trace_option(ctx, "-y store symlinks");
            return ZU_STATUS_OK;
        case 'e':
            zu_cli_error(zip_parse_tool(ctx), "encryption is not supported in this build");
            return ZU_STATUS_NOT_IMPLEMENTED;
        case 'l':
            ctx->line_mode = ZU_LINE_LF_TO_CRLF;
            zu_trace_option(ctx, "-l LF->CRLF");
            return ZU_STATUS_OK;
        default:
            break;
    }

    if (isdigit(c)) {
        ctx->compression_level = c - '0';
        zu_trace_option(ctx, "compression level %d", ctx->compression_level);
        return ZU_STATUS_OK;
    }

    return ZU_STATUS_USAGE;
}

/*
 * Parse a variable-length pattern list following -x or -i
 *
 * Conventions
 * - Consumes tokens until the next option-looking token or until "--" ends option parsing
 * - Requires at least one pattern token, otherwise returns usage error
 *
 * Arguments
 * - idx points at the option token, will be updated to the last consumed pattern
 * - endopts tracks whether "--" has been seen
 * - include selects whether patterns go to include_patterns or exclude
 */
static int parse_pattern_list(ZContext* ctx, int argc, char** argv, int* idx, bool* endopts, bool include) {
    int i = *idx + 1;
    bool any = false;

    for (; i < argc; ++i) {
        const char* tok = argv[i];

        if (!*endopts && strcmp(tok, "--") == 0) {
            *endopts = true;
            ++i;
            break;
        }

        if (!*endopts && tok[0] == '-' && tok[1] != '\0')
            break;

        if (zu_strlist_push(include ? &ctx->include_patterns : &ctx->exclude, tok) != 0)
            return ZU_STATUS_OOM;

        any = true;
    }

    if (!any) {
        zu_cli_error(zip_parse_tool(ctx), "Option requires one or more patterns");
        return ZU_STATUS_USAGE;
    }

    *idx = i - 1;
    return ZU_STATUS_OK;
}

/*
 * Like parse_pattern_list, but the first pattern may be attached to the option token
 *
 * Examples
 * -x'*.o' expands to first="*.o"
 * -iREADME expands to first="README"
 */
static int parse_pattern_list_with_first(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts, bool include) {
    if (first) {
        if (zu_strlist_push(include ? &ctx->include_patterns : &ctx->exclude, first) != 0)
            return ZU_STATUS_OOM;
        return ZU_STATUS_OK;
    }
    return parse_pattern_list(ctx, argc, argv, idx, endopts, include);
}

/*
 * Split a suffix list (colon-separated) into ctx->no_compress_suffixes
 *
 * Example
 * -n .png:.jpg:.zip
 *
 * Note
 * - This is parsed as a list of exact suffix strings, matching semantics live in the execution layer
 */
static int push_suffixes(ZContext* ctx, const char* str) {
    if (!str || !*str)
        return ZU_STATUS_OK;

    char* copy = strdup(str);
    if (!copy)
        return ZU_STATUS_OOM;

    char* saveptr;
    char* token = strtok_r(copy, ":", &saveptr);
    while (token) {
        if (zu_strlist_push(&ctx->no_compress_suffixes, token) != 0) {
            free(copy);
            return ZU_STATUS_OOM;
        }
        token = strtok_r(NULL, ":", &saveptr);
    }

    free(copy);
    return ZU_STATUS_OK;
}

/*
 * Parse -n suffix list
 * - Supports both "-nSUF1:SUF2" and "-n SUF1:SUF2"
 */
static int parse_suffix_list(ZContext* ctx, const char* first, int argc, char** argv, int* idx, bool* endopts) {
    (void)endopts;

    if (first)
        return push_suffixes(ctx, first);

    if (*idx + 1 >= argc) {
        zu_cli_error(zip_parse_tool(ctx), "-n requires suffix list");
        return ZU_STATUS_USAGE;
    }

    return push_suffixes(ctx, argv[++(*idx)]);
}

/*
 * Parse a GNU-style long option token and update ctx
 *
 * Supported forms
 * - --name
 * - --name=value
 * - --name value
 *
 * Notes
 * - This parser is intentionally explicit rather than auto-generated so it can mirror Info-ZIP aliases
 * - Unknown long options currently fall back to usage output
 */
static int parse_long_option(const char* tok, int argc, char** argv, int* idx, bool* endopts, ZContext* ctx) {
    ctx->used_long_option = true;

    const char* name = tok + 2;
    const char* value = NULL;
    char namebuf[64];

    const char* eq = strchr(name, '=');
    if (eq) {
        size_t len = (size_t)(eq - name);
        if (len >= sizeof(namebuf))
            len = sizeof(namebuf) - 1;
        memcpy(namebuf, name, len);
        namebuf[len] = '\0';
        name = namebuf;
        value = eq + 1;
    }

    // Helper macro to fetch a required argument for a long option
#define REQUIRE_ARG(optname)                                                   \
    do {                                                                       \
        if (!value) {                                                          \
            if (*idx + 1 >= argc) {                                            \
                zu_cli_error(zip_parse_tool(ctx), "%s requires an argument", optname); \
                return ZU_STATUS_USAGE;                                        \
            }                                                                  \
            value = argv[++(*idx)];                                            \
        }                                                                      \
    } while (0)

    if (strcmp(name, "dry-run") == 0) {
        ctx->dry_run = true;
        ctx->verbose = true;
        ctx->quiet = false;
        zu_trace_option(ctx, "--dry-run");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "recurse-paths") == 0) {
        ctx->recursive = true;
        zu_trace_option(ctx, "--recurse-paths");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test") == 0) {
        ctx->test_integrity = true;
        zu_trace_option(ctx, "--test");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "test-command") == 0) {
        REQUIRE_ARG("--test-command");
        free(ctx->test_command);
        ctx->test_command = strdup(value);
        if (!ctx->test_command)
            return ZU_STATUS_OOM;
        zu_trace_option(ctx, "--test-command");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "quiet") == 0) {
        ctx->quiet_level++;
        ctx->quiet = true;
        ctx->verbose = false;
        zu_trace_option(ctx, "--quiet");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "exclude") == 0) {
        int rc = value ? parse_pattern_list_with_first(ctx, value, argc, argv, idx, endopts, false) : parse_pattern_list(ctx, argc, argv, idx, endopts, false);
        if (rc == ZU_STATUS_OK)
            zu_trace_option(ctx, "--exclude");
        return rc;
    }
    if (strcmp(name, "include") == 0) {
        int rc = value ? parse_pattern_list_with_first(ctx, value, argc, argv, idx, endopts, true) : parse_pattern_list(ctx, argc, argv, idx, endopts, true);
        if (rc == ZU_STATUS_OK)
            zu_trace_option(ctx, "--include");
        return rc;
    }
    if (strcmp(name, "verbose") == 0) {
        ctx->verbose = true;
        zu_trace_option(ctx, "--verbose");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "encrypt") == 0 || strcmp(name, "password") == 0) {
        zu_cli_error(zip_parse_tool(ctx), "encryption is not supported in this build");
        return ZU_STATUS_NOT_IMPLEMENTED;
    }
    if (strcmp(name, "help") == 0) {
        print_usage(stdout, argv[0]);
        return ZU_STATUS_USAGE;
    }
    if (strcmp(name, "output-file") == 0 || strcmp(name, "out") == 0) {
        REQUIRE_ARG("--out");
        ctx->output_path = value;
        zu_trace_option(ctx, "--out=%s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "copy") == 0) {
        ctx->copy_mode = true;
        zu_trace_option(ctx, "--copy");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "la") == 0 || strcmp(name, "log-append") == 0) {
        ctx->log_append = true;
        zu_trace_option(ctx, "--log-append");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "lf") == 0 || strcmp(name, "logfile-path") == 0) {
        REQUIRE_ARG("--logfile-path");
        free(ctx->log_path);
        ctx->log_path = strdup(value);
        return ctx->log_path ? ZU_STATUS_OK : ZU_STATUS_OOM;
    }
    if (strcmp(name, "li") == 0 || strcmp(name, "log-info") == 0) {
        ctx->log_info = true;
        zu_trace_option(ctx, "--log-info");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "fast-write") == 0) {
        if (value && parse_fast_write_threshold("--fast-write", value, ctx) != ZU_STATUS_OK)
            return ZU_STATUS_USAGE;
        ctx->fast_write = true;
        zu_trace_option(ctx, "--fast-write%s", value ? value : "");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "fast-write-threshold") == 0) {
        REQUIRE_ARG("--fast-write-threshold");
        if (parse_fast_write_threshold("--fast-write-threshold", value, ctx) != ZU_STATUS_OK)
            return ZU_STATUS_USAGE;
        zu_trace_option(ctx, "--fast-write-threshold=%s", value);
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "filesync") == 0 || strcmp(name, "FS") == 0) {
        ctx->filesync = true;
        ctx->update = true;
        zu_trace_option(ctx, "--filesync");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "split-size") == 0 || strcmp(name, "pause") == 0 || strcmp(name, "sp") == 0) {
        zu_cli_error(zip_parse_tool(ctx), "split archives are not supported");
        return ZU_STATUS_NOT_IMPLEMENTED;
    }
    if (strcmp(name, "fix") == 0) {
        ctx->fix_archive = true;
        zu_trace_option(ctx, "--fix");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "FF") == 0 || strcmp(name, "fixfix") == 0) {
        ctx->fix_fix_archive = true;
        zu_trace_option(ctx, "--fixfix");
        return ZU_STATUS_OK;
    }
    if (strcmp(name, "ll") == 0) {
        ctx->line_mode = ZU_LINE_CRLF_TO_LF;
        zu_trace_option(ctx, "--ll");
        return ZU_STATUS_OK;
    }

    print_usage(stderr, argv[0]);
    return ZU_STATUS_USAGE;
}

/*
 * Parse zip/zipnote arguments into ctx
 *
 * Key conventions
 * - Option parsing stops at "--"
 * - Many short options may be clustered (-rqv), but options requiring arguments are not clusterable
 * - Multi-letter short forms (-FS, -FF, -ll, -la, -li) are handled as explicit tokens
 * - Positional parsing
 *   - First positional becomes archive_path (or "-" for stdout filter mode)
 *   - Remaining positionals become include file operands
 *
 * is_zipnote controls acceptance of zipnote-only flags (-w) and mode validation
 */
int zu_zip_parse_args(int argc, char** argv, ZContext* ctx, bool is_zipnote) {
    bool endopts = false;
    int i = 1;

    while (i < argc) {
        const char* tok = argv[i];

        if (!endopts && strcmp(tok, "--") == 0) {
            endopts = true;
            ++i;
            continue;
        }

        if (!endopts && tok[0] == '-' && tok[1] != '\0') {
            zu_trace_option(ctx, "option %s", tok);

            // Explicitly rejected combinations that are ambiguous or legacy in this implementation
            if (strcmp(tok, "-xi") == 0 || strcmp(tok, "-ix") == 0) {
                zu_cli_error(zip_parse_tool(ctx), "use -x <patterns> ... -i <patterns> instead of %s", tok);
                return ZU_STATUS_USAGE;
            }

            // Explicit stubs and unsupported flags surfaced early so users get immediate feedback
            if (strcmp(tok, "-sp") == 0) {
                zu_cli_error(zip_parse_tool(ctx), "split archives are not supported");
                return ZU_STATUS_NOT_IMPLEMENTED;
            }
            if (strcmp(tok, "-c") == 0 || strcmp(tok, "-A") == 0 || strcmp(tok, "-J") == 0) {
                zu_cli_error(zip_parse_tool(ctx), "option %s not supported in this version", tok);
                return ZU_STATUS_NOT_IMPLEMENTED;
            }

            // Multi-letter standalone tokens with special behavior
            if (strcmp(tok, "-R") == 0) {
                ctx->recursive = true;
                ctx->recurse_from_cwd = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-U") == 0) {
                ctx->copy_mode = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-FF") == 0) {
                ctx->fix_fix_archive = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-ll") == 0) {
                ctx->line_mode = ZU_LINE_CRLF_TO_LF;
                ++i;
                continue;
            }
            if (strcmp(tok, "-w") == 0) {
                if (!is_zipnote) {
                    print_usage(stderr, argv[0]);
                    return ZU_STATUS_USAGE;
                }
                ctx->zipnote_write = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-la") == 0) {
                ctx->log_append = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-li") == 0) {
                ctx->log_info = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-FS") == 0) {
                ctx->filesync = true;
                ctx->update = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-F") == 0) {
                ctx->fix_archive = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-z") == 0) {
                ctx->zip_comment_specified = true;
                ++i;
                continue;
            }
            if (strcmp(tok, "-o") == 0) {
                ctx->set_archive_mtime = true;
                ++i;
                continue;
            }

            // Options that take a separate argument token
            if (strcmp(tok, "-TT") == 0) {
                if (i + 1 >= argc) {
                    zu_cli_error(zip_parse_tool(ctx), "-TT requires a command");
                    return ZU_STATUS_USAGE;
                }
                free(ctx->test_command);
                ctx->test_command = strdup(argv[++i]);
                if (!ctx->test_command)
                    return ZU_STATUS_OOM;
                ++i;
                continue;
            }
            if (strcmp(tok, "-lf") == 0) {
                if (i + 1 >= argc) {
                    zu_cli_error(zip_parse_tool(ctx), "-lf requires a path");
                    return ZU_STATUS_USAGE;
                }
                free(ctx->log_path);
                ctx->log_path = strdup(argv[++i]);
                if (!ctx->log_path)
                    return ZU_STATUS_OOM;
                ++i;
                continue;
            }

            // -ttDATE or -tt DATE
            if (strncmp(tok, "-tt", 3) == 0) {
                const char* arg = tok[3] ? tok + 3 : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        zu_cli_error(zip_parse_tool(ctx), "-tt requires a date");
                        return ZU_STATUS_USAGE;
                    }
                    arg = argv[++i];
                }
                ctx->filter_before = parse_date(arg);
                if (ctx->filter_before == (time_t)-1) {
                    zu_cli_error(zip_parse_tool(ctx), "invalid date: %s", arg);
                    return ZU_STATUS_USAGE;
                }
                ctx->has_filter_before = true;
                ++i;
                continue;
            }

            // Long options begin with "--"
            if (tok[1] == '-') {
                int rc = parse_long_option(tok, argc, argv, &i, &endopts, ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }

            // Clusterable tokens and short options with inline argument
            char first = tok[1];
            const char* rest = tok + 2;

            // Options that take arguments: -b, -t, -P, -O, -Z
            if (first == 'b' || first == 't' || first == 'P' ||
                first == 'O' || first == 'Z') {
                const char* arg = rest[0] ? rest : NULL;
                if (!arg) {
                    if (i + 1 >= argc) {
                        zu_cli_error(zip_parse_tool(ctx), "-%c requires argument", first);
                        return ZU_STATUS_USAGE;
                    }
                    arg = argv[++i];
                }

                switch (first) {
                    case 'b':
                        free(ctx->temp_dir);
                        ctx->temp_dir = strdup(arg);
                        if (!ctx->temp_dir)
                            return ZU_STATUS_OOM;
                        break;

                    case 't':
                        ctx->filter_after = parse_date(arg);
                        if (ctx->filter_after == (time_t)-1) {
                            zu_cli_error(zip_parse_tool(ctx), "invalid date for -t");
                            return ZU_STATUS_USAGE;
                        }
                        ctx->has_filter_after = true;
                        break;

                    case 'P':
                        zu_cli_error(zip_parse_tool(ctx), "encryption is not supported in this build");
                        return ZU_STATUS_NOT_IMPLEMENTED;

                    case 'O':
                        ctx->output_path = arg;
                        break;

                    case 'Z':
                        if (strcasecmp(arg, "deflate") == 0)
                            ctx->compression_method = 8;
                        else if (strcasecmp(arg, "store") == 0)
                            ctx->compression_method = 0;
                        else if (strcasecmp(arg, "bzip2") == 0)
                            ctx->compression_method = 12;
                        else {
                            zu_cli_error(zip_parse_tool(ctx), "unknown compression method '%s'", arg);
                            return ZU_STATUS_USAGE;
                        }
                        break;
                }

                ++i;
                continue;
            }

            // Pattern list and suffix list options may consume multiple tokens
            if (strcmp(tok, "-x") == 0) {
                int rc = parse_pattern_list(ctx, argc, argv, &i, &endopts, false);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (first == 'x' && rest[0]) {
                int rc = parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, false);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (strcmp(tok, "-i") == 0) {
                int rc = parse_pattern_list(ctx, argc, argv, &i, &endopts, true);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (first == 'i' && rest[0]) {
                int rc = parse_pattern_list_with_first(ctx, rest, argc, argv, &i, &endopts, true);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (strcmp(tok, "-n") == 0) {
                int rc = parse_suffix_list(ctx, NULL, argc, argv, &i, &endopts);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (first == 'n' && rest[0]) {
                int rc = parse_suffix_list(ctx, rest, argc, argv, &i, &endopts);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }
            if (strcmp(tok, "-@") == 0) {
                ctx->stdin_names_read = true;
                int rc = read_stdin_names(ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
                ++i;
                continue;
            }

            // Guard against attaching extra characters to non-clusterable flags
            if (strchr("@FzcoAJ", first) && rest[0]) {
                zu_cli_error(zip_parse_tool(ctx), "option '%c' cannot be clustered", first);
                return ZU_STATUS_USAGE;
            }

            // Apply clustered short flags, failing on any unknown character
            bool handled = true;
            for (const char* p = tok + 1; *p; ++p) {
                if (!is_cluster_flag(*p)) {
                    handled = false;
                    break;
                }
                int rc = apply_cluster_flag(*p, ctx);
                if (rc != ZU_STATUS_OK)
                    return rc;
            }
            if (!handled) {
                print_usage(stderr, argv[0]);
                return ZU_STATUS_USAGE;
            }

            ++i;
            continue;
        }

        // Positional tokens are either archive name or file operands
        if (!ctx->archive_path) {
            ctx->archive_path = tok;

            // "-" as the archive path triggers a filter-like mode writing to stdout
            if (strcmp(ctx->archive_path, "-") == 0)
                ctx->output_to_stdout = true;
        }
        else {
            if (zu_strlist_push(&ctx->include, tok) != 0)
                return ZU_STATUS_OOM;
        }

        ++i;
    }

    /*
     * Version-only mode: -v with no archive and no file operands prints version
     * This matches Info-ZIP behavior where "zip -v" prints version and exits.
     */
    if (ctx->verbose && !ctx->archive_path && ctx->include.len == 0) {
        ctx->version_only = true;
        return ZU_STATUS_OK;
    }

    /*
     * If no archive argument was provided, decide whether to enter implicit filter mode
     *
     * - If stdin is a tty and there are no include operands, treat it as usage
     * - Otherwise assume stdin provides file data and stdout is the archive stream
     */
    if (!ctx->archive_path) {
        if (ctx->include.len == 0 && isatty(STDIN_FILENO)) {
            print_usage(stderr, argv[0]);
            return ZU_STATUS_USAGE;
        }

        // Implicit mode consumes stdin for file data
        if (ctx->stdin_names_read) {
            zu_cli_error(zip_parse_tool(ctx), "cannot use -@ with implicit stdin-to-stdout mode");
            return ZU_STATUS_USAGE;
        }

        ctx->archive_path = "-";
        ctx->output_to_stdout = true;
        if (zu_strlist_push(&ctx->include, "-") != 0)
            return ZU_STATUS_OOM;
    }

    return ZU_STATUS_OK;
}
