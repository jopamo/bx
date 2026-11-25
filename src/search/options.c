#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bx/diag.h"
#include "lib/cli_common.h"
#include "lib/option_scan.h"
#include "lib/output_policy.h"
#include "lib/size_parse.h"
#include "lib/thread_count.h"
#include "options.h"
#include "pcre2_matcher.h"
#include "record_stream.h"
#include "rg_generate.h"
#include "search.h"
#include "lib/args_common.h"

enum {
    OPT_HELP = 256,
    OPT_VERSION,
    OPT_NO_FILENAME,
    OPT_NO_IGNORE_CASE,
    OPT_NO_LINE_NUMBER,
    OPT_INCLUDE,
    OPT_EXCLUDE,
    OPT_EXCLUDE_FROM,
    OPT_EXCLUDE_DIR,
    OPT_FILES,
    OPT_TYPE_LIST,
    OPT_COLOR,
    OPT_COLUMN,
    OPT_COUNT_MATCHES,
    OPT_PASSTHRU,
    OPT_REPLACE,
    OPT_STATS,
    OPT_FILES_WITH_MATCHES,
    OPT_FILES_WITHOUT_MATCH,
    OPT_FOLLOW,
    OPT_DIRECTORIES,
    OPT_DEVICES,
    OPT_MAX_DEPTH,
    OPT_MAX_COLUMNS,
    OPT_MAX_COLUMNS_PREVIEW,
    OPT_NO_MAX_COLUMNS_PREVIEW,
    OPT_IGNORE,
    OPT_IGNORE_DOT,
    OPT_IGNORE_EXCLUDE,
    OPT_IGNORE_FILE,
    OPT_IGNORE_FILE_CASE_INSENSITIVE,
    OPT_IGNORE_FILES,
    OPT_IGNORE_GLOBAL,
    OPT_IGNORE_MESSAGES,
    OPT_IGNORE_PARENT,
    OPT_IGNORE_VCS,
    OPT_NO_IGNORE,
    OPT_NO_IGNORE_EXCLUDE,
    OPT_NO_IGNORE_FILE_CASE_INSENSITIVE,
    OPT_NO_IGNORE_FILES,
    OPT_NO_IGNORE_GLOBAL,
    OPT_NO_IGNORE_MESSAGES,
    OPT_NO_IGNORE_PARENT,
    OPT_NO_IGNORE_VCS,
    OPT_NO_IGNORE_DOT,
    OPT_NO_REQUIRE_GIT,
    OPT_REQUIRE_GIT,
    OPT_HIDDEN,
    OPT_NO_HIDDEN,
    OPT_IGLOB,
    OPT_GLOB_CASE_INSENSITIVE,
    OPT_NO_GLOB_CASE_INSENSITIVE,
    OPT_LABEL,
    OPT_GROUP_SEPARATOR,
    OPT_NO_GROUP_SEPARATOR,
    OPT_CONTEXT_SEPARATOR,
    OPT_NO_CONTEXT_SEPARATOR,
    OPT_FIELD_CONTEXT_SEPARATOR,
    OPT_FIELD_MATCH_SEPARATOR,
    OPT_HEADING,
    OPT_NO_HEADING,
    OPT_NULL,
    OPT_BINARY_FILES,
    OPT_BINARY,
    OPT_NO_BINARY,
    OPT_NULL_DATA,
    OPT_MULTILINE,
    OPT_MULTILINE_DOTALL,
    OPT_NO_MULTILINE,
    OPT_NO_MULTILINE_DOTALL,
    OPT_STOP_ON_NONMATCH,
    OPT_ENGINE,
    OPT_AUTO_HYBRID_REGEX,
    OPT_NO_AUTO_HYBRID_REGEX,
    OPT_PCRE2_VERSION,
    OPT_NO_PCRE2,
    OPT_PCRE2_UNICODE,
    OPT_NO_PCRE2_UNICODE,
    OPT_REGEX_SIZE_LIMIT,
    OPT_DFA_SIZE_LIMIT,
    OPT_ENCODING,
    OPT_NO_ENCODING,
    OPT_NO_MESSAGES,
    OPT_MESSAGES,
    OPT_JSON,
    OPT_NO_JSON,
    OPT_DEBUG,
    OPT_TRACE,
    OPT_SORT,
    OPT_SORTR,
    OPT_SORT_FILES,
    OPT_NO_SORT_FILES,
    OPT_TYPE_ADD,
    OPT_TYPE_CLEAR,
    OPT_PRE,
    OPT_NO_PRE,
    OPT_PRE_GLOB,
    OPT_SEARCH_ZIP,
    OPT_NO_SEARCH_ZIP,
    OPT_CRLF,
    OPT_NO_CRLF,
    OPT_MMAP,
    OPT_NO_MMAP,
    OPT_LINE_BUFFERED,
    OPT_NO_LINE_BUFFERED,
    OPT_BLOCK_BUFFERED,
    OPT_NO_BLOCK_BUFFERED,
    OPT_INCLUDE_ZERO,
    OPT_NO_INCLUDE_ZERO,
    OPT_NO_BYTE_OFFSET,
    OPT_NO_COLUMN,
    OPT_NO_FIXED_STRINGS,
    OPT_NO_FOLLOW,
    OPT_NO_INVERT_MATCH,
    OPT_NO_STATS,
    OPT_NO_TEXT,
    OPT_PRETTY,
    OPT_PRINT0,
    OPT_ONE_FILE_SYSTEM,
    OPT_NO_ONE_FILE_SYSTEM,
    OPT_MAX_FILESIZE,
    OPT_PATH_SEPARATOR,
    OPT_COLORS,
    OPT_NO_CONFIG,
    OPT_HOSTNAME_BIN,
    OPT_HYPERLINK_FORMAT,
    OPT_TRIM,
    OPT_NO_TRIM,
    OPT_UNICODE,
    OPT_NO_UNICODE,
    OPT_VIMGREP,
    OPT_GENERATE,
};

static const int BX_SEARCH_NUMERIC_OPTION_MAX = 1 << 20;

static bool bx_parse_nonnegative_int(const char *progname, const char *optname,
                                     const char *text, int *out) {
    if (!bx_args_parse_int_range(text, 0, BX_SEARCH_NUMERIC_OPTION_MAX, out)) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n",
                progname, optname, text ? text : "(null)");
        return false;
    }
    return true;
}

static int bx_search_parse_out_of_memory(const char *progname) {
    fprintf(stderr, "%s: out of memory\n", progname);
    return -1;
}

static const char *bx_search_parse_error_text(int errnum) {
    if (errnum == ENOMEM)
        return "Out of memory";
    if (errnum == EOVERFLOW)
        return "input record too large";
    return strerror(errnum ? errnum : EIO);
}

static int bx_search_parse_path_error(const char *progname,
                                      const char *path,
                                      int errnum) {
    fprintf(stderr, "%s: %s: %s\n",
            progname, path ? path : "", bx_search_parse_error_text(errnum));
    return -1;
}

static void bx_grep_print_usage_try_help(const char *progname) {
    fprintf(stderr, "Usage: %s [OPTION]... PATTERNS [FILE]...\n", progname);
    bx_cli_print_try_help(progname);
}

static int bx_grep_invalid_directories_mode(const char *progname, const char *value) {
    fprintf(stderr, "%s: invalid argument '%s' for '--directories'\n", progname, value);
    fputs("Valid arguments are:\n", stderr);
    fputs("  - 'read'\n", stderr);
    fputs("  - 'recurse'\n", stderr);
    fputs("  - 'skip'\n", stderr);
    bx_grep_print_usage_try_help(progname);
    return 3;
}

static int bx_grep_missing_option_argument(const char *progname, const struct bx_option_result *option) {
    if (option->short_name)
        fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, option->short_name);
    else
        fprintf(stderr, "%s: option '%s' requires an argument\n", progname, option->token);
    bx_grep_print_usage_try_help(progname);
    return -1;
}

static bool bx_grep_unsupported_required_long_option(const char *arg) {
    static const char *const names[] = {
        "--glob",
        "--iglob",
        "--replace",
        "--type",
        "--type-not",
        "--type-add",
        "--type-clear",
        "--max-depth",
        "--max-columns",
        "--context-separator",
        "--field-context-separator",
        "--field-match-separator",
        "--engine",
        "--regex-size-limit",
        "--dfa-size-limit",
        "--sort",
        "--sortr",
    };

    if (!arg)
        return false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strcmp(arg, names[i]) == 0)
            return true;
    }
    return false;
}

static int bx_grep_unrecognized_option(const char *progname, const char *arg) {
    if (arg)
        fprintf(stderr, "%s: unrecognized option '%s'\n", progname, arg);
    else
        fprintf(stderr, "%s: unrecognized option\n", progname);
    bx_grep_print_usage_try_help(progname);
    return -1;
}

static bool bx_search_personality_is_rg(enum bx_search_personality personality) {
    return personality == BX_SEARCH_RG;
}

static bool bx_search_personality_is_grep_family(enum bx_search_personality personality) {
    return personality == BX_SEARCH_GREP
        || personality == BX_SEARCH_EGREP
        || personality == BX_SEARCH_FGREP;
}

static bool bx_search_parse_sort_key(const char *text, enum bx_search_sort_key *out) {
    if (!text || !out)
        return false;
    if (strcmp(text, "none") == 0) {
        *out = BX_SEARCH_SORT_NONE;
        return true;
    }
    if (strcmp(text, "path") == 0) {
        *out = BX_SEARCH_SORT_PATH;
        return true;
    }
    if (strcmp(text, "modified") == 0) {
        *out = BX_SEARCH_SORT_MODIFIED;
        return true;
    }
    if (strcmp(text, "accessed") == 0) {
        *out = BX_SEARCH_SORT_ACCESSED;
        return true;
    }
    if (strcmp(text, "created") == 0) {
        *out = BX_SEARCH_SORT_CREATED;
        return true;
    }
    return false;
}

static int bx_search_require_rg_option(const char *progname,
                                       enum bx_search_personality personality,
                                       const char *token) {
    return bx_search_personality_is_rg(personality) ? 0 : bx_grep_unrecognized_option(progname, token);
}

static int bx_search_unsupported_rg_option(const char *progname,
                                           enum bx_search_personality personality,
                                           const char *token) {
    if (!bx_search_personality_is_rg(personality))
        return bx_grep_unrecognized_option(progname, token);
    fprintf(stderr, "%s: unsupported option '%s'\n", progname, token);
    return -1;
}

static int bx_search_reject_rg_option(const char *progname,
                                      enum bx_search_personality personality,
                                      const char *token) {
    return bx_search_personality_is_rg(personality) ? bx_grep_unrecognized_option(progname, token) : 0;
}

static bool bx_search_parse_nonnegative_int(const char *progname,
                                            enum bx_search_personality personality,
                                            const char *optname,
                                            const char *text,
                                            int *out) {
    if (personality == BX_SEARCH_GREP && strcmp(optname, "-m") == 0) {
        int parsed = 0;
        if (!bx_args_parse_int_range(text, 0, BX_SEARCH_NUMERIC_OPTION_MAX, &parsed)) {
            fprintf(stderr, "%s: invalid max count\n", progname);
            return false;
        }
        *out = parsed;
        return true;
    }

    return bx_parse_nonnegative_int(progname, optname, text, out);
}

static bool bx_rg_size_limit_parse_failed(const char *progname, const char *optname,
                                          const char *text) {
    fprintf(stderr,
            "%s: error parsing flag %s: invalid size: invalid format for size '%s', which should be a non-empty sequence of digits followed by an optional 'K', 'M' or 'G' suffix\n",
            progname, optname, text ? text : "");
    return false;
}

static bool bx_parse_rg_size_limit(const char *progname, const char *optname,
                                   const char *text, size_t *out) {
    if (!text || !*text)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    unsigned long long value = 0;
    size_t pos = 0;
    while (text[pos] >= '0' && text[pos] <= '9') {
        unsigned int digit = (unsigned int)(text[pos] - '0');
        if (value > (ULLONG_MAX - digit) / 10ULL)
            return bx_rg_size_limit_parse_failed(progname, optname, text);
        value = value * 10ULL + digit;
        pos++;
    }

    if (pos == 0)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    if (text[pos] != '\0' && (text[pos + 1] != '\0' || strchr("KMG", text[pos]) == NULL))
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    uintmax_t multiplier = 0;
    if (!bx_size_suffix_multiplier(text + pos, &multiplier))
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    if (value > ((uintmax_t)SIZE_MAX) / multiplier)
        return bx_rg_size_limit_parse_failed(progname, optname, text);

    *out = (size_t)(value * multiplier);
    return true;
}

static bool bx_set_binary_files_mode(const char *progname,
                                     enum bx_search_personality personality,
                                     struct search_opts *opts,
                                     const char *value) {
    if (strcmp(value, "binary") == 0) {
        opts->binary_as_text = false;
        opts->binary_without_match = false;
        return true;
    }
    if (strcmp(value, "text") == 0) {
        opts->binary_as_text = true;
        opts->binary_without_match = false;
        return true;
    }
    if (strcmp(value, "without-match") == 0) {
        opts->binary_as_text = false;
        opts->binary_without_match = true;
        return true;
    }

    if (bx_search_personality_is_grep_family(personality)) {
        fprintf(stderr, "%s: unknown binary-files type\n", progname);
    } else {
        fprintf(stderr, "%s: invalid argument for --binary-files: %s\n",
                progname, value);
    }
    return false;
}

void bx_search_print_help(const char *progname) {
    const char *base = bx_cli_progname(progname, "grep");
    bool is_rg = strcmp(base, "rg") == 0;

    if (!is_rg) {
        printf("Usage: %s [OPTION]... PATTERNS [FILE]...\n", progname);
        puts("Search for PATTERNS in each FILE.");
        puts("Example: grep -i 'hello world' menu.h main.c");
        puts("PATTERNS can contain multiple patterns separated by newlines.");
        puts("");
        puts("Pattern selection and interpretation:");
        puts("  -E, --extended-regexp     PATTERNS are extended regular expressions");
        puts("  -F, --fixed-strings       PATTERNS are strings");
        puts("  -G, --basic-regexp        PATTERNS are basic regular expressions");
        puts("  -P, --perl-regexp         PATTERNS are Perl regular expressions");
        puts("  -e, --regexp=PATTERNS     use PATTERNS for matching");
        puts("  -f, --file=FILE           take PATTERNS from FILE");
        puts("  -i, --ignore-case         ignore case distinctions in patterns and data");
        puts("      --no-ignore-case      do not ignore case distinctions (default)");
        puts("  -w, --word-regexp         match only whole words");
        puts("  -x, --line-regexp         match only whole lines");
        puts("  -z, --null-data           a data line ends in 0 byte, not newline");
        puts("");
        puts("Miscellaneous:");
        puts("  -s, --no-messages         suppress error messages");
        puts("  -v, --invert-match        select non-matching lines");
        puts("  -V, --version             display version information and exit");
        puts("      --help                display this help text and exit");
        puts("");
        puts("Output control:");
        puts("  -m, --max-count=NUM       stop after NUM selected lines");
        puts("  -b, --byte-offset         print the byte offset with output lines");
        puts("  -n, --line-number         print line number with output lines");
        puts("      --line-buffered       flush output on every line");
        puts("  -H, --with-filename       print file name with output lines");
        puts("  -h, --no-filename         suppress the file name prefix on output");
        puts("      --label=LABEL         use LABEL as the standard input file name prefix");
        puts("  -o, --only-matching       show only nonempty parts of lines that match");
        puts("  -q, --quiet, --silent     suppress all normal output");
        puts("      --binary-files=TYPE   assume that binary files are TYPE;");
        puts("                            TYPE is 'binary', 'text', or 'without-match'");
        puts("  -a, --text                equivalent to --binary-files=text");
        puts("  -I                        equivalent to --binary-files=without-match");
        puts("  -d, --directories=ACTION  how to handle directories;");
        puts("                            ACTION is 'read', 'recurse', or 'skip'");
        puts("  -D, --devices=ACTION      how to handle devices, FIFOs and sockets;");
        puts("                            ACTION is 'read' or 'skip'");
        puts("  -r, --recursive           like --directories=recurse");
        puts("  -R, --dereference-recursive  likewise, but follow all symlinks");
        puts("      --include=GLOB        search only files that match GLOB (a file pattern)");
        puts("      --exclude=GLOB        skip files that match GLOB");
        puts("      --exclude-from=FILE   skip files that match any file pattern from FILE");
        puts("      --exclude-dir=GLOB    skip directories that match GLOB");
        puts("  -L, --files-without-match  print only names of FILEs with no selected lines");
        puts("  -l, --files-with-matches  print only names of FILEs with selected lines");
        puts("  -c, --count               print only a count of selected lines per FILE");
        puts("  -T, --initial-tab         make tabs line up (if needed)");
        puts("  -Z, --null                print 0 byte after FILE name");
        puts("");
        puts("Context control:");
        puts("  -B, --before-context=NUM  print NUM lines of leading context");
        puts("  -A, --after-context=NUM   print NUM lines of trailing context");
        puts("  -C, --context=NUM         print NUM lines of output context");
        puts("  -NUM                      same as --context=NUM");
        puts("      --group-separator=SEP  print SEP on line between matches with context");
        puts("      --no-group-separator  do not print separator for matches with context");
        puts("      --color[=WHEN],");
        puts("      --colour[=WHEN]       use markers to highlight the matching strings;");
        puts("                            WHEN is 'always', 'never', or 'auto'");
        puts("  -U, --binary              do not strip CR characters at EOL (MSDOS/Windows)");
        puts("");
        puts("When FILE is '-', read standard input.  If no FILE is given, read standard");
        puts("input, but with -r, recursively search the working directory instead.  With");
        puts("fewer than two FILEs, assume -h.  Exit status is 0 if any line is selected,");
        puts("1 otherwise; if any error occurs and -q is not given, the exit status is 2.");
        return;
    }

    printf("Usage: %s [OPTION]... PATTERN [FILE]...\n", progname);
    puts("Search for PATTERN in each FILE.");
    puts("");
    puts("  -E, --extended-regexp  PATTERN is an extended regular expression");
    puts("  -F, --fixed-strings  PATTERN is a set of fixed strings");
    puts("  -b, --byte-offset  print the byte offset with output lines");
    puts("      --column  print the column number with output lines");
    puts("  -H, --with-filename  print the file name for each match");
    puts("      --no-filename  suppress file name prefixes");
    puts("  -h            display help and exit");
    puts("  -i, --ignore-case  ignore case distinctions");
    puts("  -n, --line-number  print line number with output lines");
    puts("      --no-line-number  suppress line numbers");
    puts("  -o, --only-matching  show only the part of a line matching PATTERN");
    puts("  -v, --invert-match  select non-matching lines");
    puts("  -c, --count  print only a count of matching lines per FILE");
    puts("      --count-matches  print only a count of individual matches per FILE");
    puts("      --passthru  print both matching and non-matching lines");
    puts("      --passthrough  print both matching and non-matching lines");
    puts("      --replace=TEXT  replace each match with TEXT in printed output");
    puts("      --stats  print a search summary after all results");
    puts("  -l, --files-with-matches  print only names of FILEs with selected lines");
    puts("  -L            print only names of FILEs with no selected lines");
    puts("  -q, --quiet   suppress all normal output");
    puts("  -s, --case-sensitive  search case-sensitively");
    puts("  -S, --smart-case  search case-insensitively when the pattern is lowercase");
    puts("  -r            recursive, do not follow symlinks");
    puts("  -R            recursive, follow symlinks");
    puts("  -a, --text    process binary files as text");
    puts("  -I            skip binary files");
    puts("  -A NUM, --after-context=NUM  print NUM lines of trailing context");
    puts("  -B NUM, --before-context=NUM  print NUM lines of leading context");
    puts("  -C NUM, --context=NUM  print NUM lines of output context");
    puts("  -Z, --null    print NUL after file names");
    puts("  -z            use NUL as the record separator");
    puts("      --label=LABEL  use LABEL as the standard input file name");
    puts("      --group-separator=SEP  use SEP between context groups");
    puts("      --no-group-separator   suppress context group separators");
    puts("      --context-separator=SEP  use SEP between ripgrep context groups");
    puts("      --no-context-separator  suppress ripgrep context group separators");
    puts("      --field-context-separator=SEP  use SEP between fields on context lines");
    puts("      --field-match-separator=SEP  use SEP between fields on matching lines");
    puts("      --heading  show file names above matches instead of as prefixes");
    puts("      --max-columns=NUM  omit long matching lines wider than NUM bytes");
    puts("      --no-heading  show file names as prefixes");
    puts("      --include=GLOB   search only files matching GLOB");
    puts("      --exclude=GLOB   skip files matching GLOB");
    puts("      --exclude-from=FILE  skip files matching patterns from FILE");
    puts("      --exclude-dir=GLOB  skip directories matching GLOB");
    puts("      --no-ignore  do not use ignore files");
    puts("      --no-ignore-parent  do not use ignore files from parent directories");
    puts("      --no-ignore-vcs  do not use VCS ignore files");
    puts("      --no-ignore-dot  do not use .ignore or .rgignore files");
    puts("      --no-require-git  use .gitignore outside git repositories");
    puts("      --hidden  search hidden files and directories");
    puts("      --iglob=GLOB  search only files matching GLOB, case-insensitively");
    puts("  -j NUM, --threads=NUM  set worker threads (0 uses available CPUs)");
    puts("  -t TYPE, --type=TYPE  search only files matching TYPE");
    puts("  -T TYPE, --type-not=TYPE  skip files matching TYPE");
    puts("  -u, --unrestricted  reduce ignore filtering");
    puts("  -w, --word-regexp  match only whole words");
    puts("  -x, --line-regexp  match only whole lines");
    puts("  -P, --pcre2  use the PCRE2 regex engine");
    puts("  -U, --multiline  allow matches to span line terminators");
    puts("      --multiline-dotall  make . match line terminators in multiline mode");
    puts("      --engine=ENGINE  choose regex engine: default, pcre2, or auto");
    puts("  -m NUM, --max-count=NUM  stop after NUM matching lines per file");
    puts("      --regex-size-limit=NUM[KMG]  accept ripgrep's regex size limit flag");
    puts("      --dfa-size-limit=NUM[KMG]  accept ripgrep's DFA size limit flag");
    puts("      --pcre2-version  print PCRE2 version information and exit");
    puts("      --stop-on-nonmatch  stop reading a file after a non-matching record follows a match");
    puts("      --sort=TYPE  sort results ascending by TYPE; supported: none, path, modified, accessed, created");
    puts("      --sortr=TYPE  sort results descending by TYPE; supported: none, path, modified, accessed, created");
    puts("  -V            output version information and exit");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

void bx_search_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

void bx_search_print_type_list(void) {
    puts("c:    *.c, *.h");
    puts("cpp:  *.cpp, *.cc, *.cxx, *.c++, *.hh, *.hpp, *.hxx, *.h++");
    puts("rs:   *.rs");
    puts("py:   *.py");
    puts("js:   *.js, *.jsx");
    puts("ts:   *.ts, *.tsx");
    puts("go:   *.go");
    puts("java: *.java");
    puts("rb:   *.rb");
    puts("sh:   *.sh, *.bash");
    puts("md:   *.md, *.markdown");
    puts("txt:  *.txt");
    puts("toml: *.toml");
    puts("json: *.json");
    puts("yaml: *.yml, *.yaml");
    puts("xml:  *.xml");
    puts("html: *.html, *.htm");
    puts("css:  *.css");
    puts("lock: *.lock");
}

static const char *bx_get_builtin_type_globs(const char *name) {
    static const struct { const char *name; const char *globs; } types[] = {
        {"c",    "*.c,*.h"},
        {"cpp",  "*.cpp,*.cc,*.cxx,*.c++,*.hh,*.hpp,*.hxx,*.h++"},
        {"rs",   "*.rs"},
        {"py",   "*.py"},
        {"js",   "*.js,*.jsx"},
        {"ts",   "*.ts,*.tsx"},
        {"go",   "*.go"},
        {"java", "*.java"},
        {"rb",   "*.rb"},
        {"sh",   "*.sh,*.bash"},
        {"md",   "*.md,*.markdown"},
        {"txt",  "*.txt"},
        {"toml", "*.toml"},
        {"json", "*.json"},
        {"yaml", "*.yml,*.yaml"},
        {"xml",  "*.xml"},
        {"html", "*.html,*.htm"},
        {"css",  "*.css"},
        {"lock", "*.lock"},
        {NULL, NULL},
    };
    for (int i = 0; types[i].name; i++)
        if (strcmp(types[i].name, name) == 0)
            return types[i].globs;
    return NULL;
}

static const char *bx_get_type_globs(const struct search_opts *opts, const char *name) {
    if (opts && name) {
        for (int i = 0; i < opts->num_cleared_types; i++) {
            if (opts->cleared_type_names[i] && strcmp(opts->cleared_type_names[i], name) == 0)
                return NULL;
        }
    }
    if (opts && name) {
        for (int i = 0; i < opts->num_custom_types; i++) {
            if (opts->custom_type_names[i] && strcmp(opts->custom_type_names[i], name) == 0)
                return opts->custom_type_globs[i];
        }
    }
    return bx_get_builtin_type_globs(name);
}

static bool bx_add_custom_type(struct search_opts *opts, const char *text) {
    if (!opts || !text)
        return false;

    const char *colon = strchr(text, ':');
    if (!colon || colon == text || colon[1] == '\0')
        return false;

    size_t name_len = (size_t)(colon - text);
    char *name = strndup(text, name_len);
    char *globs = strdup(colon + 1);
    if (!name || !globs) {
        free(name);
        free(globs);
        return false;
    }

    for (int i = 0; i < opts->num_custom_types; i++) {
        if (strcmp(opts->custom_type_names[i], name) == 0) {
            size_t merged_len = strlen(opts->custom_type_globs[i]) + 1 + strlen(globs) + 1;
            char *merged = malloc(merged_len);
            if (!merged) {
                free(name);
                free(globs);
                return false;
            }
            snprintf(merged, merged_len, "%s,%s", opts->custom_type_globs[i], globs);
            free(opts->custom_type_globs[i]);
            opts->custom_type_globs[i] = merged;
            free(name);
            free(globs);
            return true;
        }
    }

    if (opts->num_custom_types >= MAX_CUSTOM_TYPES) {
        free(name);
        free(globs);
        return false;
    }

    opts->custom_type_names[opts->num_custom_types] = name;
    opts->custom_type_globs[opts->num_custom_types] = globs;
    opts->num_custom_types++;
    return true;
}

static bool bx_clear_type(struct search_opts *opts, const char *name) {
    if (!opts || !name || *name == '\0')
        return false;

    for (int i = 0; i < opts->num_custom_types; i++) {
        if (opts->custom_type_names[i] && strcmp(opts->custom_type_names[i], name) == 0) {
            free(opts->custom_type_names[i]);
            free(opts->custom_type_globs[i]);
            for (int j = i + 1; j < opts->num_custom_types; j++) {
                opts->custom_type_names[j - 1] = opts->custom_type_names[j];
                opts->custom_type_globs[j - 1] = opts->custom_type_globs[j];
            }
            opts->num_custom_types--;
            opts->custom_type_names[opts->num_custom_types] = NULL;
            opts->custom_type_globs[opts->num_custom_types] = NULL;
            break;
        }
    }

    for (int i = 0; i < opts->num_cleared_types; i++) {
        if (opts->cleared_type_names[i] && strcmp(opts->cleared_type_names[i], name) == 0)
            return true;
    }

    if (opts->num_cleared_types >= MAX_CLEARED_TYPES)
        return false;

    opts->cleared_type_names[opts->num_cleared_types] = strdup(name);
    if (!opts->cleared_type_names[opts->num_cleared_types])
        return false;
    opts->num_cleared_types++;
    return true;
}

void bx_search_free_options(struct search_opts *opts) {
    bx_search_pattern_vec_dispose(&opts->include_patterns);
    bx_search_pattern_vec_dispose(&opts->exclude_patterns);
    bx_search_pattern_vec_dispose(&opts->exclude_dir_patterns);
    bx_search_pattern_vec_dispose(&opts->extra_patterns);
    for (int i = 0; i < opts->num_ignore_files; i++) free(opts->ignore_files[i]);
    for (int i = 0; i < opts->num_pre_globs; i++) free(opts->pre_globs[i]);
    for (int i = 0; i < opts->num_custom_types; i++) {
        free(opts->custom_type_names[i]);
        free(opts->custom_type_globs[i]);
    }
    for (int i = 0; i < opts->num_cleared_types; i++)
        free(opts->cleared_type_names[i]);
    free(opts->label);
    free(opts->group_separator);
    free(opts->replace);
    free(opts->field_match_separator);
    free(opts->field_context_separator);
    free(opts->encoding_name);
    free(opts->hostname_bin);
    free(opts->hyperlink_format);
    free(opts->pre_command);
    free(opts->operand_storage);
}

static const struct bx_option_spec long_opts[] = {
    {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
    {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
    {'e', 'e', "regexp", BX_OPTION_REQUIRED_VALUE},
    {'F', 'F', "fixed-strings", BX_OPTION_NO_VALUE},
    {'v', 'v', "invert-match", BX_OPTION_NO_VALUE},
    {'o', 'o', "only-matching", BX_OPTION_NO_VALUE},
    {OPT_INCLUDE, 0, "include", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE, 0, "exclude", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE_FROM, 0, "exclude-from", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE_DIR, 0, "exclude-dir", BX_OPTION_REQUIRED_VALUE},
    {OPT_FILES, 0, "files", BX_OPTION_NO_VALUE},
    {'c', 'c', "count", BX_OPTION_NO_VALUE},
    {OPT_COLUMN, 0, "column", BX_OPTION_NO_VALUE},
    {OPT_COUNT_MATCHES, 0, "count-matches", BX_OPTION_NO_VALUE},
    {OPT_PASSTHRU, 0, "passthru", BX_OPTION_NO_VALUE},
    {OPT_PASSTHRU, 0, "passthrough", BX_OPTION_NO_VALUE},
    {OPT_REPLACE, 0, "replace", BX_OPTION_REQUIRED_VALUE},
    {OPT_STATS, 0, "stats", BX_OPTION_NO_VALUE},
    {OPT_FILES_WITH_MATCHES, 0, "files-with-matches", BX_OPTION_NO_VALUE},
    {OPT_FILES_WITHOUT_MATCH, 0, "files-without-match", BX_OPTION_NO_VALUE},
    {OPT_DIRECTORIES, 0, "directories", BX_OPTION_REQUIRED_VALUE},
    {OPT_FOLLOW, 0, "follow", BX_OPTION_NO_VALUE},
    {'A', 'A', "after-context", BX_OPTION_REQUIRED_VALUE},
    {'B', 'B', "before-context", BX_OPTION_REQUIRED_VALUE},
    {'C', 'C', "context", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGNORE, 0, "ignore", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_DOT, 0, "ignore-dot", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_EXCLUDE, 0, "ignore-exclude", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_FILE, 0, "ignore-file", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGNORE_FILE_CASE_INSENSITIVE, 0, "ignore-file-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_FILES, 0, "ignore-files", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_GLOBAL, 0, "ignore-global", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_MESSAGES, 0, "ignore-messages", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_PARENT, 0, "ignore-parent", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_VCS, 0, "ignore-vcs", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE, 0, "no-ignore", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_EXCLUDE, 0, "no-ignore-exclude", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_FILE_CASE_INSENSITIVE, 0, "no-ignore-file-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_FILES, 0, "no-ignore-files", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_GLOBAL, 0, "no-ignore-global", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_MESSAGES, 0, "no-ignore-messages", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_PARENT, 0, "no-ignore-parent", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_VCS, 0, "no-ignore-vcs", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_DOT, 0, "no-ignore-dot", BX_OPTION_NO_VALUE},
    {OPT_NO_REQUIRE_GIT, 0, "no-require-git", BX_OPTION_NO_VALUE},
    {OPT_REQUIRE_GIT, 0, "require-git", BX_OPTION_NO_VALUE},
    {OPT_HIDDEN, 0, "hidden", BX_OPTION_NO_VALUE},
    {OPT_NO_HIDDEN, 0, "no-hidden", BX_OPTION_NO_VALUE},
    {'b', 'b', "byte-offset", BX_OPTION_NO_VALUE},
    {OPT_NO_BYTE_OFFSET, 0, "no-byte-offset", BX_OPTION_NO_VALUE},
    {'g', 'g', "glob", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGLOB, 0, "iglob", BX_OPTION_REQUIRED_VALUE},
    {OPT_GLOB_CASE_INSENSITIVE, 0, "glob-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_GLOB_CASE_INSENSITIVE, 0, "no-glob-case-insensitive", BX_OPTION_NO_VALUE},
    {'i', 'i', "ignore-case", BX_OPTION_NO_VALUE},
    {'s', 's', "case-sensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_MESSAGES, 0, "no-messages", BX_OPTION_NO_VALUE},
    {OPT_MESSAGES, 0, "messages", BX_OPTION_NO_VALUE},
    {'S', 'S', "smart-case", BX_OPTION_NO_VALUE},
    {'w', 'w', "word-regexp", BX_OPTION_NO_VALUE},
    {'x', 'x', "line-regexp", BX_OPTION_NO_VALUE},
    {'P', 'P', "pcre2", BX_OPTION_NO_VALUE},
    {OPT_NO_PCRE2, 0, "no-pcre2", BX_OPTION_NO_VALUE},
    {OPT_PCRE2_UNICODE, 0, "pcre2-unicode", BX_OPTION_NO_VALUE},
    {OPT_NO_PCRE2_UNICODE, 0, "no-pcre2-unicode", BX_OPTION_NO_VALUE},
    {'t', 't', "type", BX_OPTION_REQUIRED_VALUE},
    {'T', 'T', "type-not", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_ADD, 0, "type-add", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_CLEAR, 0, "type-clear", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_LIST, 0, "type-list", BX_OPTION_NO_VALUE},
    {'f', 'f', "file", BX_OPTION_REQUIRED_VALUE},
    {'m', 'm', "max-count", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_DEPTH, 0, "max-depth", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_DEPTH, 0, "maxdepth", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_COLUMNS, 0, "max-columns", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_COLUMNS_PREVIEW, 0, "max-columns-preview", BX_OPTION_NO_VALUE},
    {OPT_NO_MAX_COLUMNS_PREVIEW, 0, "no-max-columns-preview", BX_OPTION_NO_VALUE},
    {OPT_MAX_FILESIZE, 0, "max-filesize", BX_OPTION_REQUIRED_VALUE},
    {OPT_COLOR, 0, "color", BX_OPTION_OPTIONAL_VALUE},
    {OPT_COLOR, 0, "colour", BX_OPTION_OPTIONAL_VALUE},
    {OPT_COLORS, 0, "colors", BX_OPTION_REQUIRED_VALUE},
    {'H', 'H', "with-filename", BX_OPTION_NO_VALUE},
    {OPT_NO_FILENAME, 0, "no-filename", BX_OPTION_NO_VALUE},
    {'n', 'n', "line-number", BX_OPTION_NO_VALUE},
    {OPT_NO_LINE_NUMBER, 0, "no-line-number", BX_OPTION_NO_VALUE},
    {OPT_NO_COLUMN, 0, "no-column", BX_OPTION_NO_VALUE},
    {OPT_LABEL, 0, "label", BX_OPTION_REQUIRED_VALUE},
    {OPT_GROUP_SEPARATOR, 0, "group-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_GROUP_SEPARATOR, 0, "no-group-separator", BX_OPTION_NO_VALUE},
    {OPT_CONTEXT_SEPARATOR, 0, "context-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_CONTEXT_SEPARATOR, 0, "no-context-separator", BX_OPTION_NO_VALUE},
    {OPT_FIELD_CONTEXT_SEPARATOR, 0, "field-context-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_FIELD_MATCH_SEPARATOR, 0, "field-match-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_HEADING, 0, "heading", BX_OPTION_NO_VALUE},
    {OPT_NO_HEADING, 0, "no-heading", BX_OPTION_NO_VALUE},
    {OPT_NULL, 0, "null", BX_OPTION_NO_VALUE},
    {OPT_PRINT0, 0, "print0", BX_OPTION_NO_VALUE},
    {OPT_BINARY_FILES, 0, "binary-files", BX_OPTION_REQUIRED_VALUE},
    {OPT_BINARY, 0, "binary", BX_OPTION_NO_VALUE},
    {OPT_NO_BINARY, 0, "no-binary", BX_OPTION_NO_VALUE},
    {'a', 'a', "text", BX_OPTION_NO_VALUE},
    {OPT_NO_TEXT, 0, "no-text", BX_OPTION_NO_VALUE},
    {OPT_NULL_DATA, 0, "null-data", BX_OPTION_NO_VALUE},
    {OPT_MULTILINE, 0, "multiline", BX_OPTION_NO_VALUE},
    {OPT_MULTILINE_DOTALL, 0, "multiline-dotall", BX_OPTION_NO_VALUE},
    {OPT_NO_MULTILINE, 0, "no-multiline", BX_OPTION_NO_VALUE},
    {OPT_NO_MULTILINE_DOTALL, 0, "no-multiline-dotall", BX_OPTION_NO_VALUE},
    {OPT_STOP_ON_NONMATCH, 0, "stop-on-nonmatch", BX_OPTION_NO_VALUE},
    {OPT_JSON, 0, "json", BX_OPTION_NO_VALUE},
    {OPT_NO_JSON, 0, "no-json", BX_OPTION_NO_VALUE},
    {OPT_DEBUG, 0, "debug", BX_OPTION_NO_VALUE},
    {OPT_TRACE, 0, "trace", BX_OPTION_NO_VALUE},
    {OPT_SORT, 0, "sort", BX_OPTION_REQUIRED_VALUE},
    {OPT_SORTR, 0, "sortr", BX_OPTION_REQUIRED_VALUE},
    {OPT_SORT_FILES, 0, "sort-files", BX_OPTION_NO_VALUE},
    {OPT_NO_SORT_FILES, 0, "no-sort-files", BX_OPTION_NO_VALUE},
    {OPT_ENGINE, 0, "engine", BX_OPTION_REQUIRED_VALUE},
    {OPT_AUTO_HYBRID_REGEX, 0, "auto-hybrid-regex", BX_OPTION_NO_VALUE},
    {OPT_NO_AUTO_HYBRID_REGEX, 0, "no-auto-hybrid-regex", BX_OPTION_NO_VALUE},
    {OPT_REGEX_SIZE_LIMIT, 0, "regex-size-limit", BX_OPTION_REQUIRED_VALUE},
    {OPT_DFA_SIZE_LIMIT, 0, "dfa-size-limit", BX_OPTION_REQUIRED_VALUE},
    {OPT_ENCODING, 0, "encoding", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_ENCODING, 0, "no-encoding", BX_OPTION_NO_VALUE},
    {OPT_PCRE2_VERSION, 0, "pcre2-version", BX_OPTION_NO_VALUE},
    {'j', 'j', "threads", BX_OPTION_REQUIRED_VALUE},
    {OPT_LINE_BUFFERED, 0, "line-buffered", BX_OPTION_NO_VALUE},
    {OPT_NO_LINE_BUFFERED, 0, "no-line-buffered", BX_OPTION_NO_VALUE},
    {OPT_BLOCK_BUFFERED, 0, "block-buffered", BX_OPTION_NO_VALUE},
    {OPT_NO_BLOCK_BUFFERED, 0, "no-block-buffered", BX_OPTION_NO_VALUE},
    {OPT_MMAP, 0, "mmap", BX_OPTION_NO_VALUE},
    {OPT_NO_MMAP, 0, "no-mmap", BX_OPTION_NO_VALUE},
    {OPT_INCLUDE_ZERO, 0, "include-zero", BX_OPTION_NO_VALUE},
    {OPT_NO_INCLUDE_ZERO, 0, "no-include-zero", BX_OPTION_NO_VALUE},
    {OPT_NO_FIXED_STRINGS, 0, "no-fixed-strings", BX_OPTION_NO_VALUE},
    {OPT_NO_FOLLOW, 0, "no-follow", BX_OPTION_NO_VALUE},
    {OPT_NO_INVERT_MATCH, 0, "no-invert-match", BX_OPTION_NO_VALUE},
    {OPT_NO_STATS, 0, "no-stats", BX_OPTION_NO_VALUE},
    {OPT_PRETTY, 0, "pretty", BX_OPTION_NO_VALUE},
    {OPT_ONE_FILE_SYSTEM, 0, "one-file-system", BX_OPTION_NO_VALUE},
    {OPT_NO_ONE_FILE_SYSTEM, 0, "no-one-file-system", BX_OPTION_NO_VALUE},
    {OPT_PATH_SEPARATOR, 0, "path-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_PRE, 0, "pre", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_PRE, 0, "no-pre", BX_OPTION_NO_VALUE},
    {OPT_PRE_GLOB, 0, "pre-glob", BX_OPTION_REQUIRED_VALUE},
    {OPT_SEARCH_ZIP, 0, "search-zip", BX_OPTION_NO_VALUE},
    {OPT_NO_SEARCH_ZIP, 0, "no-search-zip", BX_OPTION_NO_VALUE},
    {OPT_CRLF, 0, "crlf", BX_OPTION_NO_VALUE},
    {OPT_NO_CRLF, 0, "no-crlf", BX_OPTION_NO_VALUE},
    {OPT_HOSTNAME_BIN, 0, "hostname-bin", BX_OPTION_REQUIRED_VALUE},
    {OPT_HYPERLINK_FORMAT, 0, "hyperlink-format", BX_OPTION_REQUIRED_VALUE},
    {OPT_TRIM, 0, "trim", BX_OPTION_NO_VALUE},
    {OPT_NO_TRIM, 0, "no-trim", BX_OPTION_NO_VALUE},
    {OPT_UNICODE, 0, "unicode", BX_OPTION_NO_VALUE},
    {OPT_NO_UNICODE, 0, "no-unicode", BX_OPTION_NO_VALUE},
    {OPT_VIMGREP, 0, "vimgrep", BX_OPTION_NO_VALUE},
    {OPT_GENERATE, 0, "generate", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_CONFIG, 0, "no-config", BX_OPTION_NO_VALUE},
    {'u', 'u', "unrestricted", BX_OPTION_NO_VALUE},
    {'0', '0', NULL, BX_OPTION_NO_VALUE},
    {'.', '.', NULL, BX_OPTION_NO_VALUE},
    {'E', 'E', NULL, BX_OPTION_REQUIRED_VALUE},
    {'h', 'h', NULL, BX_OPTION_NO_VALUE},
    {'l', 'l', NULL, BX_OPTION_NO_VALUE},
    {'L', 'L', NULL, BX_OPTION_NO_VALUE},
    {'r', 'r', NULL, BX_OPTION_REQUIRED_VALUE},
    {'R', 'R', NULL, BX_OPTION_NO_VALUE},
    {'I', 'I', NULL, BX_OPTION_NO_VALUE},
    {'z', 'z', NULL, BX_OPTION_NO_VALUE},
    {'Z', 'Z', NULL, BX_OPTION_NO_VALUE},
    {'d', 'd', NULL, BX_OPTION_REQUIRED_VALUE},
    {'M', 'M', NULL, BX_OPTION_REQUIRED_VALUE},
    {'U', 'U', NULL, BX_OPTION_NO_VALUE},
    {'V', 'V', NULL, BX_OPTION_NO_VALUE},
    {'N', 'N', NULL, BX_OPTION_NO_VALUE},
    {'p', 'p', NULL, BX_OPTION_NO_VALUE},
};

static const struct bx_option_spec grep_long_opts[] = {
    {OPT_HELP, 0, "help", BX_OPTION_NO_VALUE},
    {OPT_VERSION, 0, "version", BX_OPTION_NO_VALUE},
    {'G', 'G', "basic-regexp", BX_OPTION_NO_VALUE},
    {'E', 'E', "extended-regexp", BX_OPTION_NO_VALUE},
    {'P', 'P', "perl-regexp", BX_OPTION_NO_VALUE},
    {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
    {'q', 0, "silent", BX_OPTION_NO_VALUE},
    {'e', 'e', "regexp", BX_OPTION_REQUIRED_VALUE},
    {'F', 'F', "fixed-strings", BX_OPTION_NO_VALUE},
    {'v', 'v', "invert-match", BX_OPTION_NO_VALUE},
    {'o', 'o', "only-matching", BX_OPTION_NO_VALUE},
    {OPT_INCLUDE, 0, "include", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE, 0, "exclude", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE_FROM, 0, "exclude-from", BX_OPTION_REQUIRED_VALUE},
    {OPT_EXCLUDE_DIR, 0, "exclude-dir", BX_OPTION_REQUIRED_VALUE},
    {OPT_FILES, 0, "files", BX_OPTION_NO_VALUE},
    {'c', 'c', "count", BX_OPTION_NO_VALUE},
    {OPT_COLUMN, 0, "column", BX_OPTION_NO_VALUE},
    {OPT_COUNT_MATCHES, 0, "count-matches", BX_OPTION_NO_VALUE},
    {OPT_PASSTHRU, 0, "passthru", BX_OPTION_NO_VALUE},
    {OPT_PASSTHRU, 0, "passthrough", BX_OPTION_NO_VALUE},
    {OPT_REPLACE, 0, "replace", BX_OPTION_REQUIRED_VALUE},
    {OPT_STATS, 0, "stats", BX_OPTION_NO_VALUE},
    {OPT_FILES_WITH_MATCHES, 0, "files-with-matches", BX_OPTION_NO_VALUE},
    {OPT_FILES_WITHOUT_MATCH, 0, "files-without-match", BX_OPTION_NO_VALUE},
    {'r', 'r', "recursive", BX_OPTION_NO_VALUE},
    {'R', 'R', "dereference-recursive", BX_OPTION_NO_VALUE},
    {OPT_DIRECTORIES, 0, "directories", BX_OPTION_REQUIRED_VALUE},
    {OPT_DEVICES, 0, "devices", BX_OPTION_REQUIRED_VALUE},
    {OPT_FOLLOW, 0, "follow", BX_OPTION_NO_VALUE},
    {'A', 'A', "after-context", BX_OPTION_REQUIRED_VALUE},
    {'B', 'B', "before-context", BX_OPTION_REQUIRED_VALUE},
    {'C', 'C', "context", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGNORE, 0, "ignore", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_DOT, 0, "ignore-dot", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_EXCLUDE, 0, "ignore-exclude", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_FILE, 0, "ignore-file", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGNORE_FILE_CASE_INSENSITIVE, 0, "ignore-file-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_FILES, 0, "ignore-files", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_GLOBAL, 0, "ignore-global", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_MESSAGES, 0, "ignore-messages", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_PARENT, 0, "ignore-parent", BX_OPTION_NO_VALUE},
    {OPT_IGNORE_VCS, 0, "ignore-vcs", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE, 0, "no-ignore", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_EXCLUDE, 0, "no-ignore-exclude", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_FILE_CASE_INSENSITIVE, 0, "no-ignore-file-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_FILES, 0, "no-ignore-files", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_GLOBAL, 0, "no-ignore-global", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_MESSAGES, 0, "no-ignore-messages", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_PARENT, 0, "no-ignore-parent", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_VCS, 0, "no-ignore-vcs", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_DOT, 0, "no-ignore-dot", BX_OPTION_NO_VALUE},
    {OPT_NO_REQUIRE_GIT, 0, "no-require-git", BX_OPTION_NO_VALUE},
    {OPT_REQUIRE_GIT, 0, "require-git", BX_OPTION_NO_VALUE},
    {OPT_HIDDEN, 0, "hidden", BX_OPTION_NO_VALUE},
    {OPT_NO_HIDDEN, 0, "no-hidden", BX_OPTION_NO_VALUE},
    {'b', 'b', "byte-offset", BX_OPTION_NO_VALUE},
    {OPT_NO_BYTE_OFFSET, 0, "no-byte-offset", BX_OPTION_NO_VALUE},
    {'g', 'g', "glob", BX_OPTION_REQUIRED_VALUE},
    {OPT_IGLOB, 0, "iglob", BX_OPTION_REQUIRED_VALUE},
    {OPT_GLOB_CASE_INSENSITIVE, 0, "glob-case-insensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_GLOB_CASE_INSENSITIVE, 0, "no-glob-case-insensitive", BX_OPTION_NO_VALUE},
    {'i', 'i', "ignore-case", BX_OPTION_NO_VALUE},
    {OPT_NO_IGNORE_CASE, 0, "no-ignore-case", BX_OPTION_NO_VALUE},
    {'s', 's', "case-sensitive", BX_OPTION_NO_VALUE},
    {OPT_NO_MESSAGES, 0, "no-messages", BX_OPTION_NO_VALUE},
    {OPT_MESSAGES, 0, "messages", BX_OPTION_NO_VALUE},
    {'S', 'S', "smart-case", BX_OPTION_NO_VALUE},
    {'w', 'w', "word-regexp", BX_OPTION_NO_VALUE},
    {'x', 'x', "line-regexp", BX_OPTION_NO_VALUE},
    {'P', 0, "pcre2", BX_OPTION_NO_VALUE},
    {OPT_NO_PCRE2, 0, "no-pcre2", BX_OPTION_NO_VALUE},
    {OPT_PCRE2_UNICODE, 0, "pcre2-unicode", BX_OPTION_NO_VALUE},
    {OPT_NO_PCRE2_UNICODE, 0, "no-pcre2-unicode", BX_OPTION_NO_VALUE},
    {'t', 't', "type", BX_OPTION_REQUIRED_VALUE},
    {'T', 0, "type-not", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_ADD, 0, "type-add", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_CLEAR, 0, "type-clear", BX_OPTION_REQUIRED_VALUE},
    {OPT_TYPE_LIST, 0, "type-list", BX_OPTION_NO_VALUE},
    {'f', 'f', "file", BX_OPTION_REQUIRED_VALUE},
    {'m', 'm', "max-count", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_DEPTH, 0, "max-depth", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_DEPTH, 0, "maxdepth", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_COLUMNS, 0, "max-columns", BX_OPTION_REQUIRED_VALUE},
    {OPT_MAX_COLUMNS_PREVIEW, 0, "max-columns-preview", BX_OPTION_NO_VALUE},
    {OPT_NO_MAX_COLUMNS_PREVIEW, 0, "no-max-columns-preview", BX_OPTION_NO_VALUE},
    {OPT_MAX_FILESIZE, 0, "max-filesize", BX_OPTION_REQUIRED_VALUE},
    {OPT_COLOR, 0, "color", BX_OPTION_OPTIONAL_VALUE},
    {OPT_COLOR, 0, "colour", BX_OPTION_OPTIONAL_VALUE},
    {OPT_COLORS, 0, "colors", BX_OPTION_REQUIRED_VALUE},
    {'H', 'H', "with-filename", BX_OPTION_NO_VALUE},
    {OPT_NO_FILENAME, 0, "no-filename", BX_OPTION_NO_VALUE},
    {'n', 'n', "line-number", BX_OPTION_NO_VALUE},
    {OPT_NO_LINE_NUMBER, 0, "no-line-number", BX_OPTION_NO_VALUE},
    {OPT_NO_COLUMN, 0, "no-column", BX_OPTION_NO_VALUE},
    {'T', 'T', "initial-tab", BX_OPTION_NO_VALUE},
    {OPT_LABEL, 0, "label", BX_OPTION_REQUIRED_VALUE},
    {OPT_GROUP_SEPARATOR, 0, "group-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_GROUP_SEPARATOR, 0, "no-group-separator", BX_OPTION_NO_VALUE},
    {OPT_CONTEXT_SEPARATOR, 0, "context-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_CONTEXT_SEPARATOR, 0, "no-context-separator", BX_OPTION_NO_VALUE},
    {OPT_FIELD_CONTEXT_SEPARATOR, 0, "field-context-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_FIELD_MATCH_SEPARATOR, 0, "field-match-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_HEADING, 0, "heading", BX_OPTION_NO_VALUE},
    {OPT_NO_HEADING, 0, "no-heading", BX_OPTION_NO_VALUE},
    {OPT_NULL, 0, "null", BX_OPTION_NO_VALUE},
    {OPT_PRINT0, 0, "print0", BX_OPTION_NO_VALUE},
    {OPT_BINARY_FILES, 0, "binary-files", BX_OPTION_REQUIRED_VALUE},
    {OPT_BINARY, 0, "binary", BX_OPTION_NO_VALUE},
    {OPT_NO_BINARY, 0, "no-binary", BX_OPTION_NO_VALUE},
    {'a', 'a', "text", BX_OPTION_NO_VALUE},
    {OPT_NO_TEXT, 0, "no-text", BX_OPTION_NO_VALUE},
    {OPT_NULL_DATA, 0, "null-data", BX_OPTION_NO_VALUE},
    {OPT_MULTILINE, 0, "multiline", BX_OPTION_NO_VALUE},
    {OPT_MULTILINE_DOTALL, 0, "multiline-dotall", BX_OPTION_NO_VALUE},
    {OPT_NO_MULTILINE, 0, "no-multiline", BX_OPTION_NO_VALUE},
    {OPT_NO_MULTILINE_DOTALL, 0, "no-multiline-dotall", BX_OPTION_NO_VALUE},
    {OPT_STOP_ON_NONMATCH, 0, "stop-on-nonmatch", BX_OPTION_NO_VALUE},
    {OPT_JSON, 0, "json", BX_OPTION_NO_VALUE},
    {OPT_NO_JSON, 0, "no-json", BX_OPTION_NO_VALUE},
    {OPT_DEBUG, 0, "debug", BX_OPTION_NO_VALUE},
    {OPT_TRACE, 0, "trace", BX_OPTION_NO_VALUE},
    {OPT_SORT, 0, "sort", BX_OPTION_REQUIRED_VALUE},
    {OPT_SORTR, 0, "sortr", BX_OPTION_REQUIRED_VALUE},
    {OPT_SORT_FILES, 0, "sort-files", BX_OPTION_NO_VALUE},
    {OPT_NO_SORT_FILES, 0, "no-sort-files", BX_OPTION_NO_VALUE},
    {OPT_ENGINE, 0, "engine", BX_OPTION_REQUIRED_VALUE},
    {OPT_AUTO_HYBRID_REGEX, 0, "auto-hybrid-regex", BX_OPTION_NO_VALUE},
    {OPT_NO_AUTO_HYBRID_REGEX, 0, "no-auto-hybrid-regex", BX_OPTION_NO_VALUE},
    {OPT_REGEX_SIZE_LIMIT, 0, "regex-size-limit", BX_OPTION_REQUIRED_VALUE},
    {OPT_DFA_SIZE_LIMIT, 0, "dfa-size-limit", BX_OPTION_REQUIRED_VALUE},
    {OPT_ENCODING, 0, "encoding", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_ENCODING, 0, "no-encoding", BX_OPTION_NO_VALUE},
    {OPT_PCRE2_VERSION, 0, "pcre2-version", BX_OPTION_NO_VALUE},
    {'j', 'j', "threads", BX_OPTION_REQUIRED_VALUE},
    {OPT_LINE_BUFFERED, 0, "line-buffered", BX_OPTION_NO_VALUE},
    {OPT_NO_LINE_BUFFERED, 0, "no-line-buffered", BX_OPTION_NO_VALUE},
    {OPT_BLOCK_BUFFERED, 0, "block-buffered", BX_OPTION_NO_VALUE},
    {OPT_NO_BLOCK_BUFFERED, 0, "no-block-buffered", BX_OPTION_NO_VALUE},
    {OPT_MMAP, 0, "mmap", BX_OPTION_NO_VALUE},
    {OPT_NO_MMAP, 0, "no-mmap", BX_OPTION_NO_VALUE},
    {OPT_INCLUDE_ZERO, 0, "include-zero", BX_OPTION_NO_VALUE},
    {OPT_NO_INCLUDE_ZERO, 0, "no-include-zero", BX_OPTION_NO_VALUE},
    {OPT_NO_FIXED_STRINGS, 0, "no-fixed-strings", BX_OPTION_NO_VALUE},
    {OPT_NO_FOLLOW, 0, "no-follow", BX_OPTION_NO_VALUE},
    {OPT_NO_INVERT_MATCH, 0, "no-invert-match", BX_OPTION_NO_VALUE},
    {OPT_NO_STATS, 0, "no-stats", BX_OPTION_NO_VALUE},
    {OPT_PRETTY, 0, "pretty", BX_OPTION_NO_VALUE},
    {OPT_ONE_FILE_SYSTEM, 0, "one-file-system", BX_OPTION_NO_VALUE},
    {OPT_NO_ONE_FILE_SYSTEM, 0, "no-one-file-system", BX_OPTION_NO_VALUE},
    {OPT_PATH_SEPARATOR, 0, "path-separator", BX_OPTION_REQUIRED_VALUE},
    {OPT_PRE, 0, "pre", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_PRE, 0, "no-pre", BX_OPTION_NO_VALUE},
    {OPT_PRE_GLOB, 0, "pre-glob", BX_OPTION_REQUIRED_VALUE},
    {OPT_SEARCH_ZIP, 0, "search-zip", BX_OPTION_NO_VALUE},
    {OPT_NO_SEARCH_ZIP, 0, "no-search-zip", BX_OPTION_NO_VALUE},
    {OPT_CRLF, 0, "crlf", BX_OPTION_NO_VALUE},
    {OPT_NO_CRLF, 0, "no-crlf", BX_OPTION_NO_VALUE},
    {OPT_HOSTNAME_BIN, 0, "hostname-bin", BX_OPTION_REQUIRED_VALUE},
    {OPT_HYPERLINK_FORMAT, 0, "hyperlink-format", BX_OPTION_REQUIRED_VALUE},
    {OPT_TRIM, 0, "trim", BX_OPTION_NO_VALUE},
    {OPT_NO_TRIM, 0, "no-trim", BX_OPTION_NO_VALUE},
    {OPT_UNICODE, 0, "unicode", BX_OPTION_NO_VALUE},
    {OPT_NO_UNICODE, 0, "no-unicode", BX_OPTION_NO_VALUE},
    {OPT_VIMGREP, 0, "vimgrep", BX_OPTION_NO_VALUE},
    {OPT_GENERATE, 0, "generate", BX_OPTION_REQUIRED_VALUE},
    {OPT_NO_CONFIG, 0, "no-config", BX_OPTION_NO_VALUE},
    {'u', 'u', "unrestricted", BX_OPTION_NO_VALUE},
    {'0', '0', NULL, BX_OPTION_NO_VALUE},
    {'.', '.', NULL, BX_OPTION_NO_VALUE},
    {'h', 'h', NULL, BX_OPTION_NO_VALUE},
    {'l', 'l', NULL, BX_OPTION_NO_VALUE},
    {'L', 'L', NULL, BX_OPTION_NO_VALUE},
    {'I', 'I', NULL, BX_OPTION_NO_VALUE},
    {'z', 'z', NULL, BX_OPTION_NO_VALUE},
    {'Z', 'Z', NULL, BX_OPTION_NO_VALUE},
    {'D', 'D', NULL, BX_OPTION_REQUIRED_VALUE},
    {'d', 'd', NULL, BX_OPTION_REQUIRED_VALUE},
    {'M', 'M', NULL, BX_OPTION_REQUIRED_VALUE},
    {'U', 'U', NULL, BX_OPTION_NO_VALUE},
    {'V', 'V', NULL, BX_OPTION_NO_VALUE},
    {'N', 'N', NULL, BX_OPTION_NO_VALUE},
    {'p', 'p', NULL, BX_OPTION_NO_VALUE},
};

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                            enum bx_search_personality personality,
                            struct bx_search_operands *operands) {
    memset(opts, 0, sizeof(*opts));
    const char *progname = bx_cli_progname(argv[0], "grep");

    if (personality == BX_SEARCH_EGREP) opts->extended_regex = true;
    if (personality == BX_SEARCH_FGREP) opts->fixed_strings = true;
    opts->max_depth = -1;
    opts->encoding_mode = BX_RG_ENCODING_AUTO;
    opts->path_separator = '/';
    opts->unicode = true;
    opts->color_mode = BX_COLOR_AUTO;
    bx_color_set_mode(opts->color_mode);
    bx_rg_color_settings_init_defaults(&opts->rg_colors);
    bx_rg_parse_hyperlink_format(progname, "none", &opts->hyperlink_format);
    if (personality == BX_SEARCH_RG) {
        opts->recursive = true;
        opts->follow_symlinks = false;
        opts->binary_without_match = true;
        opts->hidden = false;
        opts->no_ignore = false;
        opts->smart_case = false;
    } else {
        opts->hidden = true;
        opts->no_ignore = true;
        opts->unicode = false;
        opts->locale_word_regexp = true;
    }

    bool rg = bx_search_personality_is_rg(personality);
    const struct bx_option_spec *specs = rg ? long_opts : grep_long_opts;
    size_t spec_count = rg ? sizeof(long_opts) / sizeof(*long_opts) : sizeof(grep_long_opts) / sizeof(*grep_long_opts);
    int operand_count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, argc > 0 ? 1u : 0u, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, spec_count);
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        const char *token = option.token;
        if (option.event == BX_OPTION_OPERAND) {
            if (!opts->operand_storage) {
                opts->operand_storage = calloc((size_t)argc - option.index, sizeof(*opts->operand_storage));
                if (!opts->operand_storage)
                    return bx_search_parse_out_of_memory(progname);
            }
            opts->operand_storage[operand_count++] = argv[option.index];
            continue;
        }
        if (option.event == BX_OPTION_MISSING_VALUE) {
            if (!rg) {
                if (option.spec->long_name && !option.short_name && bx_grep_unsupported_required_long_option(token))
                    return bx_grep_unrecognized_option(progname, token);
                return bx_grep_missing_option_argument(progname, &option);
            }
            if (option.short_name)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, option.short_name);
            else
                fprintf(stderr, "%s: option requires an argument -- '%s'\n", progname, token);
            return -1;
        }
        if (option.event != BX_OPTION_MATCH) {
            /* grep accepts a standalone legacy -NUM context count. */
            if (!rg && option.event == BX_OPTION_UNKNOWN && token[1] >= '1' && token[1] <= '9' &&
                token[1 + strspn(token + 1, "0123456789")] == '\0') {
                int n;
                if (!bx_parse_nonnegative_int(progname, token, token + 1, &n))
                    return -1;
                opts->after_context = n;
                opts->before_context = n;
                opts->context_requested = true;
                bx_option_cursor_init(&cursor, argv, option.index + 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE, specs, spec_count);
                continue;
            }
            if (option.short_name)
                fprintf(stderr, "%s: invalid option -- '%c'\n", progname, option.short_name);
            else
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, token);
            if (personality == BX_SEARCH_GREP)
                bx_grep_print_usage_try_help(progname);
            return -1;
        }
        int c = option.spec->id;
        char *value = option.value;
        const char *long_name = option.short_name ? NULL : option.spec->long_name;
        switch (c) {
        case '0':
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->null_output = true;
            break;
        case '.':
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->hidden = true;
            break;
        case 'E':
            if (bx_search_personality_is_rg(personality)) {
                if (long_name && strcmp(long_name, "extended-regexp") == 0)
                    return bx_grep_unrecognized_option(progname, token);
                if (!bx_rg_parse_encoding_name(progname, value, &opts->encoding_mode,
                                               &opts->encoding_name)) {
                    return -1;
                }
                break;
            }
            opts->extended_regex = true;
            opts->fixed_strings = false;
            opts->perl_regexp = false;
            break;
        case 'G':
            if (bx_search_personality_is_rg(personality)) {
                fprintf(stderr, "%s: invalid option -- 'G'\n", progname);
                return -1;
            }
            opts->extended_regex = false;
            opts->fixed_strings = false;
            opts->perl_regexp = false;
            break;
        case 'F':
            opts->fixed_strings = true;
            opts->extended_regex = false;
            opts->perl_regexp = false;
            break;
        case 'b': opts->show_byte_offset = true; break;
        case OPT_NO_BYTE_OFFSET:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case OPT_COLUMN:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->show_column = true;
            break;
        case OPT_NO_COLUMN:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case 'H':
            opts->show_filename = true;
            opts->hide_filename = false;
            break;
        case OPT_NO_FILENAME:
            opts->hide_filename = true;
            opts->show_filename = false;
            break;
        case 'h':
            if (bx_search_personality_is_rg(personality)) {
                bx_search_print_help(progname);
                return 1;
            }
            opts->hide_filename = true;
            break;
        case 'i':
            opts->ignore_case = true;
            if (bx_search_personality_is_rg(personality))
                opts->smart_case = false;
            break;
        case OPT_NO_IGNORE_CASE:
            opts->ignore_case = false;
            if (bx_search_personality_is_rg(personality))
                opts->smart_case = false;
            break;
        case 'n': opts->show_line_number = true; break;
        case 'N':
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->show_line_number = false;
            break;
        case OPT_NO_LINE_NUMBER:
            opts->show_line_number = false;
            break;
        case 'o': opts->only_matching = true; break;
        case 'v': opts->invert_match = true; break;
        case OPT_NO_INVERT_MATCH:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->invert_match = false;
            break;
        case 'c': opts->count_only = true; break;
        case OPT_COUNT_MATCHES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->count_matches = true;
            opts->count_only = true;
            break;
        case OPT_PASSTHRU:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->passthru = true;
            break;
        case OPT_REPLACE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->replace);
            opts->replace = strdup(value);
            if (!opts->replace)
                return -1;
            break;
        case OPT_STATS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->stats = true;
            break;
        case OPT_NO_STATS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->stats = false;
            break;
        case 'l': opts->files_with_matches = true; break;
        case 'L':
            if (bx_search_personality_is_rg(personality))
                opts->follow_symlinks = true;
            else
                opts->files_without_match = true;
            break;
        case 'q': opts->quiet = true; break;
        case 'p':
        case OPT_PRETTY:
            if (bx_search_require_rg_option(
                    progname, personality, token) != 0) {
                return -1;
            }
            opts->color_mode = BX_COLOR_ALWAYS;
            bx_color_set_mode(opts->color_mode);
            opts->heading = true;
            opts->heading_set = true;
            opts->show_line_number = true;
            break;
        case 'r':
            if (bx_search_personality_is_rg(personality)) {
                if (long_name && strcmp(long_name, "recursive") == 0)
                    return bx_grep_unrecognized_option(progname, token);
                free(opts->replace);
                opts->replace = strdup(value);
                if (!opts->replace)
                    return -1;
            } else {
                opts->recursive = true;
                opts->follow_symlinks = false;
            }
            break;
        case 'R':
            if (bx_search_personality_is_rg(personality)) {
                fprintf(stderr, "%s: unrecognized flag %s\n",
                        progname, token ? token : "-R");
                return -1;
            }
            opts->recursive = true;
            opts->follow_symlinks = true;
            break;
        case 'd':
        case OPT_DIRECTORIES:
            if (bx_search_personality_is_rg(personality)) {
                if (c == OPT_DIRECTORIES
                    && long_name && strcmp(long_name, "directories") == 0) {
                    fprintf(stderr, "%s: unrecognized flag --directories\n", progname);
                    return -1;
                }
                if (!bx_parse_nonnegative_int(progname, "-d", value, &opts->max_depth))
                    return -1;
            } else {
                if (strcmp(value, "read") == 0) {
                    opts->directory_mode = BX_GREP_DIR_READ;
                } else if (strcmp(value, "recurse") == 0) {
                    opts->directory_mode = BX_GREP_DIR_RECURSE;
                    opts->recursive = true;
                    opts->follow_symlinks = false;
                } else if (strcmp(value, "skip") == 0) {
                    opts->directory_mode = BX_GREP_DIR_SKIP;
                } else {
                    if (bx_search_personality_is_grep_family(personality)) {
                        return bx_grep_invalid_directories_mode(progname, value);
                    }
                    fprintf(stderr, "%s: invalid argument for -d: %s\n", progname, value);
                    return -1;
                }
            }
            break;
        case 'D':
        case OPT_DEVICES:
            if (bx_search_personality_is_rg(personality)) {
                if (long_name && strcmp(long_name, "devices") == 0) {
                    fprintf(stderr, "%s: unrecognized flag --devices\n", progname);
                    return -1;
                }
                return bx_search_require_rg_option(progname, personality, token);
            }
            if (strcmp(value, "read") == 0) {
                opts->device_mode = BX_GREP_DEVICE_READ;
            } else if (strcmp(value, "skip") == 0) {
                opts->device_mode = BX_GREP_DEVICE_SKIP;
            } else {
                fprintf(stderr, "%s: invalid argument '%s' for '--devices'\n",
                        progname, value);
                fputs("Valid arguments are:\n", stderr);
                fputs("  - 'read'\n", stderr);
                fputs("  - 'skip'\n", stderr);
                bx_grep_print_usage_try_help(progname);
                return 3;
            }
            opts->device_mode_explicit = true;
            break;
        case OPT_MAX_COLUMNS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "--max-columns", value, &opts->max_columns))
                return -1;
            break;
        case 'M':
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "-M", value, &opts->max_columns))
                return -1;
            break;
        case OPT_MAX_COLUMNS_PREVIEW:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case 'I':
            if (bx_search_personality_is_rg(personality)) {
                opts->hide_filename = true;
                opts->show_filename = false;
            } else {
                opts->binary_without_match = true;
                opts->binary_as_text = false;
            }
            break;
        case 'a':
            opts->binary_as_text = true;
            opts->binary_without_match = false;
            break;
        case OPT_NO_TEXT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->binary_as_text = false;
            opts->binary_without_match = true;
            break;
        case OPT_BINARY:
            if (bx_search_personality_is_rg(personality)) {
                opts->binary_as_text = false;
                opts->binary_without_match = false;
            } else {
                opts->crlf = false;
            }
            break;
        case OPT_NO_BINARY:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->binary_as_text = false;
            opts->binary_without_match = true;
            break;
        case 'w': opts->word_regexp = true; break;
        case 'x': opts->line_regexp = true; break;
        case 'P':
            if (bx_search_personality_is_rg(personality)) {
                if (long_name && strcmp(long_name, "perl-regexp") == 0)
                    return bx_grep_unrecognized_option(progname, token);
            }
            opts->perl_regexp = true;
            opts->extended_regex = false;
            opts->fixed_strings = false;
            break;
        case OPT_NO_PCRE2:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->perl_regexp = false;
            if (opts->rg_engine == BX_RG_ENGINE_PCRE2
                || opts->rg_engine == BX_RG_ENGINE_UNSPECIFIED) {
                opts->rg_engine = BX_RG_ENGINE_DEFAULT;
            }
            break;
        case 's':
            if (bx_search_personality_is_rg(personality)) {
                opts->smart_case = false;
                opts->ignore_case = false;
            } else {
                if (long_name && strcmp(long_name, "case-sensitive") == 0)
                    return bx_grep_unrecognized_option(progname, token);
                opts->suppress_errors = true;
            }
            break;
        case 'S':
            if (bx_search_personality_is_rg(personality)) {
                opts->smart_case = true;
                opts->ignore_case = false;
            } else {
                fprintf(stderr, "%s: invalid option -- 'S'\n", progname);
                return -1;
            }
            break;
        case 'm':
            if (!bx_search_parse_nonnegative_int(progname, personality, "-m", value, &opts->max_count))
                return -1;
            break;
        case 'Z':
            if (bx_search_personality_is_rg(personality)) {
                fprintf(stderr, "%s: invalid option -- 'Z'\n", progname);
                return -1;
            }
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case 'z':
            if (bx_search_personality_is_rg(personality)) {
                free(opts->pre_command);
                opts->pre_command = NULL;
                opts->search_zip = true;
                break;
            }
            opts->null_data = true;
            break;
        case 'g':
            if (bx_search_personality_is_rg(personality) && value && value[0] == '!') {
                if (!bx_search_pattern_vec_append(&opts->exclude_patterns,
                                                  value + 1,
                                                  opts->glob_case_insensitive,
                                                  false)) {
                    return bx_search_parse_out_of_memory(progname);
                }
            } else if (!bx_search_pattern_vec_append(&opts->include_patterns,
                                                     value,
                                                     opts->glob_case_insensitive,
                                                     false)) {
                return bx_search_parse_out_of_memory(progname);
            }
            break;
        case OPT_IGLOB:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (value && value[0] == '!') {
                fprintf(stderr, "%s: unsupported option argument for --iglob: %s\n", progname, value);
                return -1;
            }
            if (!bx_search_pattern_vec_append(&opts->include_patterns,
                                              value, true, false))
                return bx_search_parse_out_of_memory(progname);
            break;
        case 'u':
            if (opts->unrestrict_level < 3) opts->unrestrict_level++;
            break;
        case 'U':
            if (!bx_search_personality_is_rg(personality)) {
                opts->crlf = false;
                break;
            }
            opts->multiline = true;
            break;
        case OPT_NO_MULTILINE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->multiline = false;
            break;
        case OPT_HIDDEN:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->hidden = true;
            break;
        case OPT_NO_HIDDEN:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->hidden = false;
            break;
        case 'j':
            if (!bx_thread_count_parse(progname, "--threads", value, &opts->threads))
                return -1;
            break;
        case 't':
        case 'T': {
            if (bx_search_personality_is_rg(personality) && c == 'T'
                && long_name && strcmp(long_name, "initial-tab") == 0) {
                return bx_grep_unrecognized_option(progname, token);
            }
            if (!bx_search_personality_is_rg(personality)) {
                if (c == 'T' && (!long_name || strcmp(long_name, "initial-tab") == 0)) {
                    opts->initial_tab = true;
                    break;
                }
                if (long_name)
                    return bx_grep_unrecognized_option(progname, token);
                fprintf(stderr, "%s: invalid option -- '%c'\n", progname, c);
                bx_grep_print_usage_try_help(progname);
                return -1;
            }
            const char *globs = bx_get_type_globs(opts, value);
            if (!globs) {
                if (bx_search_personality_is_rg(personality)) {
                    fprintf(stderr, "%s: unrecognized file type: %s\n", progname, value);
                    return -1;
                }
                break;
            }
            char *copy = strdup(globs);
            if (!copy)
                return bx_search_parse_out_of_memory(progname);
            char *tok = strtok(copy, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                struct bx_search_pattern_vec *patterns =
                    c == 't' ? &opts->include_patterns : &opts->exclude_patterns;
                if (!bx_search_pattern_vec_append(patterns, tok,
                                                  opts->glob_case_insensitive,
                                                  true)) {
                    free(copy);
                    return bx_search_parse_out_of_memory(progname);
                }
                tok = strtok(NULL, ",");
            }
            free(copy);
            break;
        }
        case OPT_TYPE_ADD:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_add_custom_type(opts, value)) {
                fprintf(stderr, "%s: invalid argument for --type-add: %s\n", progname, value);
                return -1;
            }
            break;
        case OPT_TYPE_CLEAR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_clear_type(opts, value)) {
                fprintf(stderr, "%s: invalid argument for --type-clear: %s\n", progname, value);
                return -1;
            }
            break;
        case 'e':
            if (!bx_search_pattern_vec_append(&opts->extra_patterns,
                                              value, false, false))
                return bx_search_parse_out_of_memory(progname);
            break;
        case 'f': {
            FILE *pf = NULL;
            bool close_pf = false;
            int read_errno = 0;
            struct bx_record_stream pattern_stream = {0};
            if (strcmp(value, "-") == 0) {
                pf = stdin;
            } else {
                pf = fopen(value, "r");
                close_pf = true;
            }
            if (!pf) {
                fprintf(stderr, "%s: %s: %s\n", progname, value, strerror(errno));
                return -1;
            }
            bx_record_stream_begin(pf, &pattern_stream);
            for (;;) {
                ssize_t line_len;

                line_len = bx_record_stream_read(pf, &pattern_stream, '\n');
                if (line_len < 0) {
                    if (bx_record_stream_had_error(&pattern_stream))
                        read_errno = bx_record_stream_error(&pattern_stream);
                    break;
                }
                char *line = pattern_stream.record;
                size_t llen = strlen(line);
                while (llen > 0 && line[llen-1] == '\n')
                    line[--llen] = '\0';
                if (!bx_search_pattern_vec_append(&opts->extra_patterns,
                                                  line, false, false)) {
                    read_errno = ENOMEM;
                    break;
                }
            }
            if (close_pf && fclose(pf) != 0 && read_errno == 0)
                read_errno = errno != 0 ? errno : EIO;
            bx_record_stream_dispose(&pattern_stream);
            if (read_errno != 0)
                return bx_search_parse_path_error(progname, value, read_errno);
            break;
        }
        case 'A':
            if (!bx_parse_nonnegative_int(progname, "-A", value, &opts->after_context))
                return -1;
            if (bx_search_personality_is_grep_family(personality))
                opts->context_requested = true;
            break;
        case 'B':
            if (!bx_parse_nonnegative_int(progname, "-B", value, &opts->before_context))
                return -1;
            if (bx_search_personality_is_grep_family(personality))
                opts->context_requested = true;
            break;
        case 'C': {
            int n;
            if (!bx_parse_nonnegative_int(progname, "-C", value, &n))
                return -1;
            opts->after_context = n;
            opts->before_context = n;
            if (bx_search_personality_is_grep_family(personality))
                opts->context_requested = true;
            break;
        }
        case OPT_INCLUDE:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--include");
            if (!bx_search_pattern_vec_append(&opts->include_patterns,
                                              value, false, false))
                return bx_search_parse_out_of_memory(progname);
            break;
        case OPT_EXCLUDE:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude");
            if (!bx_search_pattern_vec_append(&opts->exclude_patterns,
                                              value, false, false))
                return bx_search_parse_out_of_memory(progname);
            break;
        case OPT_EXCLUDE_FROM: {
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude-from");
            FILE *ef = fopen(value, "r");
            int read_errno = 0;
            struct bx_record_stream exclude_stream = {0};
            if (!ef) {
                fprintf(stderr, "%s: %s: %s\n", progname, value, strerror(errno));
                return -1;
            }
            bx_record_stream_begin(ef, &exclude_stream);
            for (;;) {
                ssize_t line_len;

                line_len = bx_record_stream_read(ef, &exclude_stream, '\n');
                if (line_len < 0) {
                    if (bx_record_stream_had_error(&exclude_stream))
                        read_errno = bx_record_stream_error(&exclude_stream);
                    break;
                }
                char *line = exclude_stream.record;
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                    line[--len] = '\0';
                if (len == 0)
                    continue;
                if (!bx_search_pattern_vec_append(&opts->exclude_patterns,
                                                  line, false, false)) {
                    read_errno = ENOMEM;
                    break;
                }
            }
            if (fclose(ef) != 0 && read_errno == 0)
                read_errno = errno != 0 ? errno : EIO;
            bx_record_stream_dispose(&exclude_stream);
            if (read_errno != 0)
                return bx_search_parse_path_error(progname, value, read_errno);
            break;
        }
        case OPT_EXCLUDE_DIR:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude-dir");
            if (!bx_search_pattern_vec_append(&opts->exclude_dir_patterns,
                                              value, false, false))
                return bx_search_parse_out_of_memory(progname);
            break;
        case OPT_FILES:
            opts->files_only = true;
            break;
        case OPT_FILES_WITH_MATCHES:
            opts->files_with_matches = true;
            break;
        case OPT_FILES_WITHOUT_MATCH:
            opts->files_without_match = true;
            break;
        case OPT_FOLLOW:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->follow_symlinks = true;
            break;
        case OPT_NO_FOLLOW:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->follow_symlinks = false;
            break;
        case OPT_IGNORE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore = false;
            break;
        case OPT_IGNORE_PARENT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_parent = false;
            break;
        case OPT_IGNORE_VCS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_vcs = false;
            break;
        case OPT_IGNORE_DOT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_dot = false;
            break;
        case OPT_IGNORE_EXCLUDE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_exclude = false;
            break;
        case OPT_IGNORE_FILES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_files = false;
            break;
        case OPT_IGNORE_GLOBAL:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_global = false;
            break;
        case OPT_IGNORE_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->suppress_ignore_messages = false;
            break;
        case OPT_IGNORE_FILE_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->ignore_file_case_insensitive = true;
            break;
        case OPT_IGNORE_FILE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!opts->no_ignore_files && opts->num_ignore_files < MAX_RG_IGNORE_FILES)
                opts->ignore_files[opts->num_ignore_files++] = strdup(value);
            break;
        case OPT_GLOB_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->glob_case_insensitive = true;
            break;
        case OPT_NO_IGNORE:
            if (!bx_search_personality_is_rg(personality)) {
                bx_grep_print_usage_try_help(progname);
                return -1;
            }
            opts->no_ignore = true;
            break;
        case OPT_NO_IGNORE_PARENT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_parent = true;
            break;
        case OPT_NO_IGNORE_VCS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_vcs = true;
            break;
        case OPT_NO_IGNORE_DOT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_dot = true;
            break;
        case OPT_NO_IGNORE_EXCLUDE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_exclude = true;
            break;
        case OPT_NO_IGNORE_FILES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_files = true;
            break;
        case OPT_NO_IGNORE_GLOBAL:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_ignore_global = true;
            break;
        case OPT_NO_IGNORE_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->suppress_ignore_messages = true;
            break;
        case OPT_NO_IGNORE_FILE_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->ignore_file_case_insensitive = false;
            break;
        case OPT_NO_GLOB_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->glob_case_insensitive = false;
            break;
        case OPT_NO_REQUIRE_GIT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_require_git = true;
            break;
        case OPT_REQUIRE_GIT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->no_require_git = false;
            break;
        case OPT_TYPE_LIST:
            bx_search_print_type_list();
            return 1;
        case OPT_PRINT0:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->null_output = true;
            break;
        case OPT_MAX_DEPTH:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "--max-depth", value, &opts->max_depth))
                return -1;
            break;
        case OPT_LABEL:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--label");
            free(opts->label);
            opts->label = strdup(value);
            break;
        case OPT_GROUP_SEPARATOR:
            if (bx_search_reject_rg_option(progname, personality, token) != 0) {
                return -1;
            }
            free(opts->group_separator);
            opts->group_separator = strdup(value);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_GROUP_SEPARATOR:
            if (bx_search_reject_rg_option(progname, personality, token) != 0) {
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_CONTEXT_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->group_separator);
            opts->group_separator = strdup(value);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_CONTEXT_SEPARATOR:
            if (!bx_search_personality_is_rg(personality)) {
                fprintf(stderr, "%s: unrecognized option '--no-context-separator'\n", progname);
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_FIELD_CONTEXT_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->field_context_separator);
            opts->field_context_separator = strdup(value);
            break;
        case OPT_FIELD_MATCH_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->field_match_separator);
            opts->field_match_separator = strdup(value);
            break;
        case OPT_HEADING:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->heading = true;
            opts->heading_set = true;
            break;
        case OPT_NO_HEADING:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->heading = false;
            opts->heading_set = true;
            break;
        case OPT_NULL:
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case OPT_BINARY_FILES:
            if (bx_search_reject_rg_option(progname, personality, token) != 0) {
                return -1;
            }
            if (!bx_set_binary_files_mode(progname, personality, opts, value))
                return -1;
            break;
        case OPT_NULL_DATA:
            opts->null_data = true;
            break;
        case OPT_NO_MESSAGES:
            opts->suppress_errors = true;
            break;
        case OPT_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->suppress_errors = false;
            break;
        case OPT_MULTILINE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->multiline = true;
            break;
        case OPT_MULTILINE_DOTALL:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->multiline = true;
            opts->multiline_dotall = true;
            break;
        case OPT_NO_MULTILINE_DOTALL:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->multiline_dotall = false;
            break;
        case OPT_STOP_ON_NONMATCH:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->stop_on_nonmatch = true;
            break;
        case OPT_JSON: {
            struct bx_output_policy output_policy;

            bx_output_policy_init_stdout(&output_policy);
            bx_output_policy_set_json(&output_policy, true);
            if (!bx_output_policy_json_requested(&output_policy)) {
                return -1;
            }
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        }
        case OPT_NO_JSON:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case OPT_DEBUG:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            fprintf(stderr, "%s: unsupported option '--debug'\n", progname);
            return -1;
        case OPT_TRACE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->trace = true;
            break;
        case OPT_SORT:
        case OPT_SORTR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_search_parse_sort_key(value, &opts->sort_key)) {
                fprintf(stderr,
                        "%s: unsupported argument for %s: %s\n",
                        progname,
                        c == OPT_SORT ? "--sort" : "--sortr",
                        value);
                return -1;
            }
            opts->sort_dir = c == OPT_SORTR
                                 ? BX_SEARCH_SORT_DESCENDING
                                 : BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_SORT_FILES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->sort_key = BX_SEARCH_SORT_PATH;
            opts->sort_dir = BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_NO_SORT_FILES:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->sort_key = BX_SEARCH_SORT_NONE;
            opts->sort_dir = BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_ENGINE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (strcmp(value, "default") == 0)
                opts->rg_engine = BX_RG_ENGINE_DEFAULT;
            else if (strcmp(value, "pcre2") == 0)
                opts->rg_engine = BX_RG_ENGINE_PCRE2;
            else if (strcmp(value, "auto") == 0)
                opts->rg_engine = BX_RG_ENGINE_AUTO;
            else {
                fprintf(stderr,
                        "%s: error parsing flag --engine: unrecognized regex engine '%s'\n",
                        progname, value);
                return -1;
            }
            break;
        case OPT_AUTO_HYBRID_REGEX:
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        case OPT_NO_AUTO_HYBRID_REGEX:
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        case OPT_PCRE2_VERSION:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->pcre2_version = true;
            bx_regex_print_version();
            return 1;
        case OPT_REGEX_SIZE_LIMIT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--regex-size-limit", value,
                                        &opts->regex_size_limit))
                return -1;
            opts->regex_size_limit_set = true;
            break;
        case OPT_DFA_SIZE_LIMIT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--dfa-size-limit", value,
                                        &opts->dfa_size_limit))
                return -1;
            opts->dfa_size_limit_set = true;
            break;
        case OPT_ENCODING:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_rg_parse_encoding_name(progname, value, &opts->encoding_mode,
                                           &opts->encoding_name))
                return -1;
            break;
        case OPT_NO_ENCODING:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->encoding_name);
            opts->encoding_name = NULL;
            opts->encoding_mode = BX_RG_ENCODING_AUTO;
            break;
        case OPT_COLOR:
            if (bx_search_personality_is_rg(personality)
                && long_name && strcmp(long_name, "colour") == 0) {
                fprintf(stderr, "%s: unrecognized flag --colour\n", progname);
                return -1;
            }
            opts->color_mode = bx_color_parse(value ? value : "auto");
            bx_color_set_mode(opts->color_mode);
            break;
        case OPT_COLORS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_rg_parse_colors_spec(progname, value, &opts->rg_colors))
                return -1;
            break;
        case OPT_CRLF:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->crlf = true;
            opts->null_data = false;
            break;
        case OPT_NO_CRLF:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->crlf = false;
            break;
        case OPT_HOSTNAME_BIN:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->hostname_bin);
            opts->hostname_bin = NULL;
            if (value[0] != '\0') {
                opts->hostname_bin = strdup(value);
                if (!opts->hostname_bin)
                    return -1;
            }
            break;
        case OPT_HYPERLINK_FORMAT:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_rg_parse_hyperlink_format(progname, value,
                                              &opts->hyperlink_format)) {
                return -1;
            }
            break;
        case OPT_NO_MAX_COLUMNS_PREVIEW:
            if (bx_search_require_rg_option(
                    progname, personality, token) != 0) {
                return -1;
            }
            break;
        case OPT_NO_PCRE2_UNICODE:
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        case OPT_PCRE2_UNICODE:
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        case OPT_PATH_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_rg_parse_path_separator(progname, value,
                                            &opts->path_separator)) {
                return -1;
            }
            break;
        case OPT_PRE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            if (value[0] != '\0') {
                opts->pre_command = strdup(value);
                if (!opts->pre_command)
                    return -1;
                opts->search_zip = false;
            }
            break;
        case OPT_NO_PRE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            break;
        case OPT_PRE_GLOB:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (opts->num_pre_globs < MAX_PRE_GLOBS) {
                opts->pre_globs[opts->num_pre_globs] = strdup(value);
                if (!opts->pre_globs[opts->num_pre_globs])
                    return -1;
                opts->num_pre_globs++;
            }
            break;
        case OPT_SEARCH_ZIP:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            opts->search_zip = true;
            break;
        case OPT_NO_SEARCH_ZIP:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->search_zip = false;
            break;
        case OPT_TRIM:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->trim = true;
            break;
        case OPT_NO_TRIM:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->trim = false;
            break;
        case OPT_UNICODE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->unicode = true;
            break;
        case OPT_NO_UNICODE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->unicode = false;
            break;
        case OPT_LINE_BUFFERED:
            opts->line_buffered = true;
            break;
        case OPT_NO_LINE_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->line_buffered = false;
            break;
        case OPT_BLOCK_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case OPT_NO_BLOCK_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case OPT_MMAP:
        case OPT_NO_MMAP:
            if (bx_search_require_rg_option(
                    progname, personality, token) != 0) {
                return -1;
            }
            break;
        case OPT_INCLUDE_ZERO:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->include_zero = true;
            break;
        case OPT_NO_INCLUDE_ZERO:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->include_zero = false;
            break;
        case OPT_NO_FIXED_STRINGS:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->fixed_strings = false;
            break;
        case OPT_ONE_FILE_SYSTEM:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->stay_on_filesystem = true;
            break;
        case OPT_NO_ONE_FILE_SYSTEM:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->stay_on_filesystem = false;
            break;
        case OPT_MAX_FILESIZE:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--max-filesize", value,
                                        &opts->max_filesize))
                return -1;
            opts->max_filesize_set = true;
            break;
        case OPT_VIMGREP:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            opts->vimgrep = true;
            break;
        case OPT_GENERATE:
            return bx_search_unsupported_rg_option(
                progname, personality, token);
        case OPT_NO_CONFIG:
            if (bx_search_require_rg_option(progname, personality, token) != 0)
                return -1;
            break;
        case OPT_HELP:
            bx_search_print_help(progname);
            return 1;
        case 'V':
        case OPT_VERSION:
            bx_search_print_version(progname);
            return 1;
        default:
            return -1;
        }
    }

    if (opts->unrestrict_level >= 1) opts->no_ignore = true;
    if (opts->unrestrict_level >= 2) opts->hidden = true;
    if (opts->unrestrict_level >= 3) {
        opts->binary_as_text = true;
        opts->binary_without_match = false;
    }
    if (opts->null_data) {
        opts->binary_as_text = true;
        opts->binary_without_match = false;
        opts->crlf = false;
    }
    struct bx_output_policy output_policy;
    bx_output_policy_init_stdout(&output_policy);
    bx_output_policy_apply_color_mode(&output_policy, opts->color_mode);
    bx_output_policy_allow_raw_exception(&output_policy,
                                         BX_OUTPUT_POLICY_RAW_MATCHED_CONTENT);
    if (opts->null_output || opts->null_filename || opts->null_data) {
        bx_output_policy_set_nul_terminated(&output_policy, true);
        bx_output_policy_allow_raw_exception(
            &output_policy,
            BX_OUTPUT_POLICY_RAW_MACHINE_DELIMITED_PATHS);
    }
    opts->terminal_quote_paths =
        bx_output_policy_terminal_quote_paths(&output_policy);

    if (bx_search_personality_is_rg(personality)) {
        if (opts->files_with_matches || opts->files_without_match)
            opts->count_only = false;
        if (opts->count_only)
            opts->omit_zero_count_output = !opts->include_zero;
        if (opts->show_column)
            opts->show_line_number = true;
        if (opts->perl_regexp)
            opts->rg_engine = BX_RG_ENGINE_PCRE2;
        if (opts->rg_engine == BX_RG_ENGINE_PCRE2)
            opts->perl_regexp = true;
        if (opts->color_mode == BX_COLOR_ALWAYS)
            bx_color_set_mode(opts->color_mode);
    }

    int first_file = 0;
    if (opts->files_only) {
        operands->pattern = "";
    } else if (opts->extra_patterns.len > 0u) {
        operands->pattern = opts->extra_patterns.items[opts->extra_patterns.len - 1u];
        bx_search_pattern_vec_drop_last(&opts->extra_patterns);
    }
    else if (operand_count == 0) {
        if (personality == BX_SEARCH_GREP)
            bx_grep_print_usage_try_help(progname);
        else
            fprintf(stderr, "%s: ripgrep requires at least one pattern to execute a search\n", progname);
        return -1;
    }
    else {
        operands->pattern = opts->operand_storage[first_file++];
    }
    operands->files = opts->operand_storage ? opts->operand_storage + first_file : NULL;
    operands->num_files = operand_count - first_file;
    return 0;
}
