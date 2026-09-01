#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "bx/diag.h"
#include "lib/cli_common.h"
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

static int bx_grep_missing_option_argument(const char *progname, int missing_opt,
                                           int parse_optind, int argc, char **argv) {
    if (parse_optind > 0 && parse_optind <= argc && argv[parse_optind - 1] != NULL
        && strncmp(argv[parse_optind - 1], "--", 2) == 0) {
        fprintf(stderr, "%s: option '%s' requires an argument\n", progname, argv[parse_optind - 1]);
    } else if (missing_opt != 0) {
        fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, missing_opt);
    } else {
        fprintf(stderr, "%s: option requires an argument\n", progname);
    }
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

static const char *bx_search_current_option_token(int option_index, int argc, char **argv,
                                                  const char *fallback) {
    if (option_index > 0 && option_index <= argc && argv[option_index - 1] != NULL)
        return argv[option_index - 1];
    return fallback;
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
                                       int option_index, int argc, char **argv,
                                       const char *fallback) {
    if (bx_search_personality_is_rg(personality))
        return 0;
    return bx_grep_unrecognized_option(
        progname,
        bx_search_current_option_token(option_index, argc, argv, fallback));
}

static int bx_search_unsupported_rg_option(const char *progname,
                                           enum bx_search_personality personality,
                                           int option_index, int argc, char **argv,
                                           const char *fallback) {
    if (!bx_search_personality_is_rg(personality)) {
        return bx_grep_unrecognized_option(
            progname,
            bx_search_current_option_token(option_index, argc, argv, fallback));
    }

    fprintf(stderr, "%s: unsupported option '%s'\n",
            progname,
            bx_search_current_option_token(option_index, argc, argv, fallback));
    return -1;
}

static int bx_search_reject_rg_option(const char *progname,
                                      enum bx_search_personality personality,
                                      int option_index, int argc, char **argv,
                                      const char *fallback) {
    if (!bx_search_personality_is_rg(personality))
        return 0;
    return bx_grep_unrecognized_option(
        progname,
        bx_search_current_option_token(option_index, argc, argv, fallback));
}

static bool bx_search_current_option_is_long(int option_index, int argc, char **argv,
                                             const char *prefix) {
    const char *token = bx_search_current_option_token(option_index, argc, argv, NULL);
    if (!token || strncmp(token, "--", 2) != 0)
        return false;
    if (!prefix)
        return true;
    size_t prefix_len = strlen(prefix);
    return strncmp(token, prefix, prefix_len) == 0;
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
    for (int i = 0; i < opts->num_include; i++) free(opts->include_patterns[i]);
    for (int i = 0; i < opts->num_exclude; i++) free(opts->exclude_patterns[i]);
    for (int i = 0; i < opts->num_exclude_dir; i++) free(opts->exclude_dir_patterns[i]);
    for (int i = 0; i < 16; i++) free(opts->extra_patterns[i]);
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
}

static struct option long_opts[] = {
    {"help",         no_argument,       NULL, OPT_HELP},
    {"version",      no_argument,       NULL, OPT_VERSION},
    {"quiet",        no_argument,       NULL, 'q'},
    {"regexp",       required_argument, NULL, 'e'},
    {"fixed-strings", no_argument,      NULL, 'F'},
    {"invert-match", no_argument,       NULL, 'v'},
    {"only-matching", no_argument,      NULL, 'o'},
    {"include",      required_argument, NULL, OPT_INCLUDE},
    {"exclude",      required_argument, NULL, OPT_EXCLUDE},
    {"exclude-from", required_argument, NULL, OPT_EXCLUDE_FROM},
    {"exclude-dir",  required_argument, NULL, OPT_EXCLUDE_DIR},
    {"files",        no_argument,       NULL, OPT_FILES},
    {"count",        no_argument,       NULL, 'c'},
    {"column",       no_argument,       NULL, OPT_COLUMN},
    {"count-matches", no_argument,      NULL, OPT_COUNT_MATCHES},
    {"passthru",     no_argument,       NULL, OPT_PASSTHRU},
    {"passthrough",  no_argument,       NULL, OPT_PASSTHRU},
    {"replace",      required_argument, NULL, OPT_REPLACE},
    {"stats",        no_argument,       NULL, OPT_STATS},
    {"files-with-matches", no_argument, NULL, OPT_FILES_WITH_MATCHES},
    {"files-without-match", no_argument, NULL, OPT_FILES_WITHOUT_MATCH},
    {"directories", required_argument, NULL, OPT_DIRECTORIES},
    {"follow",       no_argument,       NULL, OPT_FOLLOW},
    {"after-context", required_argument, NULL, 'A'},
    {"before-context", required_argument, NULL, 'B'},
    {"context",      required_argument, NULL, 'C'},
    {"ignore", no_argument,             NULL, OPT_IGNORE},
    {"ignore-dot", no_argument,         NULL, OPT_IGNORE_DOT},
    {"ignore-exclude", no_argument,     NULL, OPT_IGNORE_EXCLUDE},
    {"ignore-file", required_argument,  NULL, OPT_IGNORE_FILE},
    {"ignore-file-case-insensitive", no_argument, NULL, OPT_IGNORE_FILE_CASE_INSENSITIVE},
    {"ignore-files", no_argument,       NULL, OPT_IGNORE_FILES},
    {"ignore-global", no_argument,      NULL, OPT_IGNORE_GLOBAL},
    {"ignore-messages", no_argument,    NULL, OPT_IGNORE_MESSAGES},
    {"ignore-parent", no_argument,      NULL, OPT_IGNORE_PARENT},
    {"ignore-vcs", no_argument,         NULL, OPT_IGNORE_VCS},
    {"no-ignore", no_argument,          NULL, OPT_NO_IGNORE},
    {"no-ignore-exclude", no_argument,  NULL, OPT_NO_IGNORE_EXCLUDE},
    {"no-ignore-file-case-insensitive", no_argument, NULL, OPT_NO_IGNORE_FILE_CASE_INSENSITIVE},
    {"no-ignore-files", no_argument,    NULL, OPT_NO_IGNORE_FILES},
    {"no-ignore-global", no_argument,   NULL, OPT_NO_IGNORE_GLOBAL},
    {"no-ignore-messages", no_argument, NULL, OPT_NO_IGNORE_MESSAGES},
    {"no-ignore-parent", no_argument,   NULL, OPT_NO_IGNORE_PARENT},
    {"no-ignore-vcs", no_argument,      NULL, OPT_NO_IGNORE_VCS},
    {"no-ignore-dot", no_argument,      NULL, OPT_NO_IGNORE_DOT},
    {"no-require-git", no_argument,     NULL, OPT_NO_REQUIRE_GIT},
    {"require-git", no_argument,        NULL, OPT_REQUIRE_GIT},
    {"hidden",       no_argument,       NULL, OPT_HIDDEN},
    {"no-hidden",    no_argument,       NULL, OPT_NO_HIDDEN},
    {"byte-offset",  no_argument,       NULL, 'b'},
    {"no-byte-offset", no_argument,     NULL, OPT_NO_BYTE_OFFSET},
    {"glob",         required_argument, NULL, 'g'},
    {"iglob",        required_argument, NULL, OPT_IGLOB},
    {"glob-case-insensitive", no_argument, NULL, OPT_GLOB_CASE_INSENSITIVE},
    {"no-glob-case-insensitive", no_argument, NULL, OPT_NO_GLOB_CASE_INSENSITIVE},
    {"ignore-case",  no_argument,       NULL, 'i'},
    {"case-sensitive", no_argument,     NULL, 's'},
    {"no-messages",  no_argument,       NULL, OPT_NO_MESSAGES},
    {"messages",     no_argument,       NULL, OPT_MESSAGES},
    {"smart-case",   no_argument,       NULL, 'S'},
    {"word-regexp",  no_argument,       NULL, 'w'},
    {"line-regexp",  no_argument,       NULL, 'x'},
    {"pcre2",        no_argument,       NULL, 'P'},
    {"no-pcre2",     no_argument,       NULL, OPT_NO_PCRE2},
    {"pcre2-unicode", no_argument,      NULL, OPT_PCRE2_UNICODE},
    {"no-pcre2-unicode", no_argument,   NULL, OPT_NO_PCRE2_UNICODE},
    {"type",         required_argument, NULL, 't'},
    {"type-not",     required_argument, NULL, 'T'},
    {"type-add",     required_argument, NULL, OPT_TYPE_ADD},
    {"type-clear",   required_argument, NULL, OPT_TYPE_CLEAR},
    {"type-list",    no_argument,       NULL, OPT_TYPE_LIST},
    {"file",         required_argument, NULL, 'f'},
    {"max-count",    required_argument, NULL, 'm'},
    {"max-depth",    required_argument, NULL, OPT_MAX_DEPTH},
    {"maxdepth",     required_argument, NULL, OPT_MAX_DEPTH},
    {"max-columns",  required_argument, NULL, OPT_MAX_COLUMNS},
    {"max-columns-preview", no_argument, NULL, OPT_MAX_COLUMNS_PREVIEW},
    {"no-max-columns-preview", no_argument, NULL, OPT_NO_MAX_COLUMNS_PREVIEW},
    {"max-filesize", required_argument, NULL, OPT_MAX_FILESIZE},
    {"color",        optional_argument, NULL, OPT_COLOR},
    {"colour",       optional_argument, NULL, OPT_COLOR},
    {"colors",       required_argument, NULL, OPT_COLORS},
    {"with-filename", no_argument,      NULL, 'H'},
    {"no-filename",  no_argument,       NULL, OPT_NO_FILENAME},
    {"line-number",  no_argument,       NULL, 'n'},
    {"no-line-number", no_argument,     NULL, OPT_NO_LINE_NUMBER},
    {"no-column",    no_argument,       NULL, OPT_NO_COLUMN},
    {"label",        required_argument, NULL, OPT_LABEL},
    {"group-separator", required_argument, NULL, OPT_GROUP_SEPARATOR},
    {"no-group-separator", no_argument, NULL, OPT_NO_GROUP_SEPARATOR},
    {"context-separator", required_argument, NULL, OPT_CONTEXT_SEPARATOR},
    {"no-context-separator", no_argument, NULL, OPT_NO_CONTEXT_SEPARATOR},
    {"field-context-separator", required_argument, NULL, OPT_FIELD_CONTEXT_SEPARATOR},
    {"field-match-separator", required_argument, NULL, OPT_FIELD_MATCH_SEPARATOR},
    {"heading",      no_argument,       NULL, OPT_HEADING},
    {"no-heading",   no_argument,       NULL, OPT_NO_HEADING},
    {"null",         no_argument,       NULL, OPT_NULL},
    {"print0",       no_argument,       NULL, OPT_PRINT0},
    {"binary-files", required_argument, NULL, OPT_BINARY_FILES},
    {"binary",       no_argument,       NULL, OPT_BINARY},
    {"no-binary",    no_argument,       NULL, OPT_NO_BINARY},
    {"text",         no_argument,       NULL, 'a'},
    {"no-text",      no_argument,       NULL, OPT_NO_TEXT},
    {"null-data",    no_argument,       NULL, OPT_NULL_DATA},
    {"multiline",    no_argument,       NULL, OPT_MULTILINE},
    {"multiline-dotall", no_argument,   NULL, OPT_MULTILINE_DOTALL},
    {"no-multiline", no_argument,       NULL, OPT_NO_MULTILINE},
    {"no-multiline-dotall", no_argument, NULL, OPT_NO_MULTILINE_DOTALL},
    {"stop-on-nonmatch", no_argument,   NULL, OPT_STOP_ON_NONMATCH},
    {"json",         no_argument,       NULL, OPT_JSON},
    {"no-json",      no_argument,       NULL, OPT_NO_JSON},
    {"debug",        no_argument,       NULL, OPT_DEBUG},
    {"trace",        no_argument,       NULL, OPT_TRACE},
    {"sort",         required_argument, NULL, OPT_SORT},
    {"sortr",        required_argument, NULL, OPT_SORTR},
    {"sort-files",   no_argument,       NULL, OPT_SORT_FILES},
    {"no-sort-files", no_argument,      NULL, OPT_NO_SORT_FILES},
    {"engine",       required_argument, NULL, OPT_ENGINE},
    {"auto-hybrid-regex", no_argument,  NULL, OPT_AUTO_HYBRID_REGEX},
    {"no-auto-hybrid-regex", no_argument, NULL, OPT_NO_AUTO_HYBRID_REGEX},
    {"regex-size-limit", required_argument, NULL, OPT_REGEX_SIZE_LIMIT},
    {"dfa-size-limit", required_argument, NULL, OPT_DFA_SIZE_LIMIT},
    {"encoding",     required_argument, NULL, OPT_ENCODING},
    {"no-encoding",  no_argument,       NULL, OPT_NO_ENCODING},
    {"pcre2-version", no_argument,      NULL, OPT_PCRE2_VERSION},
    {"threads",      required_argument, NULL, 'j'},
    {"line-buffered", no_argument,      NULL, OPT_LINE_BUFFERED},
    {"no-line-buffered", no_argument,   NULL, OPT_NO_LINE_BUFFERED},
    {"block-buffered", no_argument,     NULL, OPT_BLOCK_BUFFERED},
    {"no-block-buffered", no_argument,  NULL, OPT_NO_BLOCK_BUFFERED},
    {"mmap",         no_argument,       NULL, OPT_MMAP},
    {"no-mmap",      no_argument,       NULL, OPT_NO_MMAP},
    {"include-zero", no_argument,       NULL, OPT_INCLUDE_ZERO},
    {"no-include-zero", no_argument,    NULL, OPT_NO_INCLUDE_ZERO},
    {"no-fixed-strings", no_argument,   NULL, OPT_NO_FIXED_STRINGS},
    {"no-follow",    no_argument,       NULL, OPT_NO_FOLLOW},
    {"no-invert-match", no_argument,    NULL, OPT_NO_INVERT_MATCH},
    {"no-stats",     no_argument,       NULL, OPT_NO_STATS},
    {"pretty",       no_argument,       NULL, OPT_PRETTY},
    {"one-file-system", no_argument,    NULL, OPT_ONE_FILE_SYSTEM},
    {"no-one-file-system", no_argument, NULL, OPT_NO_ONE_FILE_SYSTEM},
    {"path-separator", required_argument, NULL, OPT_PATH_SEPARATOR},
    {"pre",          required_argument, NULL, OPT_PRE},
    {"no-pre",       no_argument,       NULL, OPT_NO_PRE},
    {"pre-glob",     required_argument, NULL, OPT_PRE_GLOB},
    {"search-zip",   no_argument,       NULL, OPT_SEARCH_ZIP},
    {"no-search-zip", no_argument,      NULL, OPT_NO_SEARCH_ZIP},
    {"crlf",         no_argument,       NULL, OPT_CRLF},
    {"no-crlf",      no_argument,       NULL, OPT_NO_CRLF},
    {"hostname-bin", required_argument, NULL, OPT_HOSTNAME_BIN},
    {"hyperlink-format", required_argument, NULL, OPT_HYPERLINK_FORMAT},
    {"trim",         no_argument,       NULL, OPT_TRIM},
    {"no-trim",      no_argument,       NULL, OPT_NO_TRIM},
    {"unicode",      no_argument,       NULL, OPT_UNICODE},
    {"no-unicode",   no_argument,       NULL, OPT_NO_UNICODE},
    {"vimgrep",      no_argument,       NULL, OPT_VIMGREP},
    {"generate",     required_argument, NULL, OPT_GENERATE},
    {"no-config",    no_argument,       NULL, OPT_NO_CONFIG},
    {"unrestricted", no_argument,       NULL, 'u'},
    {NULL, 0, NULL, 0},
};

static struct option grep_long_opts[] = {
    {"help",         no_argument,       NULL, OPT_HELP},
    {"version",      no_argument,       NULL, OPT_VERSION},
    {"basic-regexp", no_argument,       NULL, 'G'},
    {"extended-regexp", no_argument,    NULL, 'E'},
    {"perl-regexp",  no_argument,       NULL, 'P'},
    {"quiet",        no_argument,       NULL, 'q'},
    {"silent",       no_argument,       NULL, 'q'},
    {"regexp",       required_argument, NULL, 'e'},
    {"fixed-strings", no_argument,      NULL, 'F'},
    {"invert-match", no_argument,       NULL, 'v'},
    {"only-matching", no_argument,      NULL, 'o'},
    {"include",      required_argument, NULL, OPT_INCLUDE},
    {"exclude",      required_argument, NULL, OPT_EXCLUDE},
    {"exclude-from", required_argument, NULL, OPT_EXCLUDE_FROM},
    {"exclude-dir",  required_argument, NULL, OPT_EXCLUDE_DIR},
    {"files",        no_argument,       NULL, OPT_FILES},
    {"count",        no_argument,       NULL, 'c'},
    {"column",       no_argument,       NULL, OPT_COLUMN},
    {"count-matches", no_argument,      NULL, OPT_COUNT_MATCHES},
    {"passthru",     no_argument,       NULL, OPT_PASSTHRU},
    {"passthrough",  no_argument,       NULL, OPT_PASSTHRU},
    {"replace",      required_argument, NULL, OPT_REPLACE},
    {"stats",        no_argument,       NULL, OPT_STATS},
    {"files-with-matches", no_argument, NULL, OPT_FILES_WITH_MATCHES},
    {"files-without-match", no_argument, NULL, OPT_FILES_WITHOUT_MATCH},
    {"recursive",    no_argument,       NULL, 'r'},
    {"dereference-recursive", no_argument, NULL, 'R'},
    {"directories", required_argument, NULL, OPT_DIRECTORIES},
    {"devices",      required_argument, NULL, OPT_DEVICES},
    {"follow",       no_argument,       NULL, OPT_FOLLOW},
    {"after-context", required_argument, NULL, 'A'},
    {"before-context", required_argument, NULL, 'B'},
    {"context",      required_argument, NULL, 'C'},
    {"ignore", no_argument,             NULL, OPT_IGNORE},
    {"ignore-dot", no_argument,         NULL, OPT_IGNORE_DOT},
    {"ignore-exclude", no_argument,     NULL, OPT_IGNORE_EXCLUDE},
    {"ignore-file", required_argument,  NULL, OPT_IGNORE_FILE},
    {"ignore-file-case-insensitive", no_argument, NULL, OPT_IGNORE_FILE_CASE_INSENSITIVE},
    {"ignore-files", no_argument,       NULL, OPT_IGNORE_FILES},
    {"ignore-global", no_argument,      NULL, OPT_IGNORE_GLOBAL},
    {"ignore-messages", no_argument,    NULL, OPT_IGNORE_MESSAGES},
    {"ignore-parent", no_argument,      NULL, OPT_IGNORE_PARENT},
    {"ignore-vcs", no_argument,         NULL, OPT_IGNORE_VCS},
    {"no-ignore", no_argument,          NULL, OPT_NO_IGNORE},
    {"no-ignore-exclude", no_argument,  NULL, OPT_NO_IGNORE_EXCLUDE},
    {"no-ignore-file-case-insensitive", no_argument, NULL, OPT_NO_IGNORE_FILE_CASE_INSENSITIVE},
    {"no-ignore-files", no_argument,    NULL, OPT_NO_IGNORE_FILES},
    {"no-ignore-global", no_argument,   NULL, OPT_NO_IGNORE_GLOBAL},
    {"no-ignore-messages", no_argument, NULL, OPT_NO_IGNORE_MESSAGES},
    {"no-ignore-parent", no_argument,   NULL, OPT_NO_IGNORE_PARENT},
    {"no-ignore-vcs", no_argument,      NULL, OPT_NO_IGNORE_VCS},
    {"no-ignore-dot", no_argument,      NULL, OPT_NO_IGNORE_DOT},
    {"no-require-git", no_argument,     NULL, OPT_NO_REQUIRE_GIT},
    {"require-git", no_argument,        NULL, OPT_REQUIRE_GIT},
    {"hidden",       no_argument,       NULL, OPT_HIDDEN},
    {"no-hidden",    no_argument,       NULL, OPT_NO_HIDDEN},
    {"byte-offset",  no_argument,       NULL, 'b'},
    {"no-byte-offset", no_argument,     NULL, OPT_NO_BYTE_OFFSET},
    {"glob",         required_argument, NULL, 'g'},
    {"iglob",        required_argument, NULL, OPT_IGLOB},
    {"glob-case-insensitive", no_argument, NULL, OPT_GLOB_CASE_INSENSITIVE},
    {"no-glob-case-insensitive", no_argument, NULL, OPT_NO_GLOB_CASE_INSENSITIVE},
    {"ignore-case",  no_argument,       NULL, 'i'},
    {"no-ignore-case", no_argument,     NULL, OPT_NO_IGNORE_CASE},
    {"case-sensitive", no_argument,     NULL, 's'},
    {"no-messages",  no_argument,       NULL, OPT_NO_MESSAGES},
    {"messages",     no_argument,       NULL, OPT_MESSAGES},
    {"smart-case",   no_argument,       NULL, 'S'},
    {"word-regexp",  no_argument,       NULL, 'w'},
    {"line-regexp",  no_argument,       NULL, 'x'},
    {"pcre2",        no_argument,       NULL, 'P'},
    {"no-pcre2",     no_argument,       NULL, OPT_NO_PCRE2},
    {"pcre2-unicode", no_argument,      NULL, OPT_PCRE2_UNICODE},
    {"no-pcre2-unicode", no_argument,   NULL, OPT_NO_PCRE2_UNICODE},
    {"type",         required_argument, NULL, 't'},
    {"type-not",     required_argument, NULL, 'T'},
    {"type-add",     required_argument, NULL, OPT_TYPE_ADD},
    {"type-clear",   required_argument, NULL, OPT_TYPE_CLEAR},
    {"type-list",    no_argument,       NULL, OPT_TYPE_LIST},
    {"file",         required_argument, NULL, 'f'},
    {"max-count",    required_argument, NULL, 'm'},
    {"max-depth",    required_argument, NULL, OPT_MAX_DEPTH},
    {"maxdepth",     required_argument, NULL, OPT_MAX_DEPTH},
    {"max-columns",  required_argument, NULL, OPT_MAX_COLUMNS},
    {"max-columns-preview", no_argument, NULL, OPT_MAX_COLUMNS_PREVIEW},
    {"no-max-columns-preview", no_argument, NULL, OPT_NO_MAX_COLUMNS_PREVIEW},
    {"max-filesize", required_argument, NULL, OPT_MAX_FILESIZE},
    {"color",        optional_argument, NULL, OPT_COLOR},
    {"colour",       optional_argument, NULL, OPT_COLOR},
    {"colors",       required_argument, NULL, OPT_COLORS},
    {"with-filename", no_argument,      NULL, 'H'},
    {"no-filename",  no_argument,       NULL, OPT_NO_FILENAME},
    {"line-number",  no_argument,       NULL, 'n'},
    {"no-line-number", no_argument,     NULL, OPT_NO_LINE_NUMBER},
    {"no-column",    no_argument,       NULL, OPT_NO_COLUMN},
    {"initial-tab",  no_argument,       NULL, 'T'},
    {"label",        required_argument, NULL, OPT_LABEL},
    {"group-separator", required_argument, NULL, OPT_GROUP_SEPARATOR},
    {"no-group-separator", no_argument, NULL, OPT_NO_GROUP_SEPARATOR},
    {"context-separator", required_argument, NULL, OPT_CONTEXT_SEPARATOR},
    {"no-context-separator", no_argument, NULL, OPT_NO_CONTEXT_SEPARATOR},
    {"field-context-separator", required_argument, NULL, OPT_FIELD_CONTEXT_SEPARATOR},
    {"field-match-separator", required_argument, NULL, OPT_FIELD_MATCH_SEPARATOR},
    {"heading",      no_argument,       NULL, OPT_HEADING},
    {"no-heading",   no_argument,       NULL, OPT_NO_HEADING},
    {"null",         no_argument,       NULL, OPT_NULL},
    {"print0",       no_argument,       NULL, OPT_PRINT0},
    {"binary-files", required_argument, NULL, OPT_BINARY_FILES},
    {"binary",       no_argument,       NULL, OPT_BINARY},
    {"no-binary",    no_argument,       NULL, OPT_NO_BINARY},
    {"text",         no_argument,       NULL, 'a'},
    {"no-text",      no_argument,       NULL, OPT_NO_TEXT},
    {"null-data",    no_argument,       NULL, OPT_NULL_DATA},
    {"multiline",    no_argument,       NULL, OPT_MULTILINE},
    {"multiline-dotall", no_argument,   NULL, OPT_MULTILINE_DOTALL},
    {"no-multiline", no_argument,       NULL, OPT_NO_MULTILINE},
    {"no-multiline-dotall", no_argument, NULL, OPT_NO_MULTILINE_DOTALL},
    {"stop-on-nonmatch", no_argument,   NULL, OPT_STOP_ON_NONMATCH},
    {"json",         no_argument,       NULL, OPT_JSON},
    {"no-json",      no_argument,       NULL, OPT_NO_JSON},
    {"debug",        no_argument,       NULL, OPT_DEBUG},
    {"trace",        no_argument,       NULL, OPT_TRACE},
    {"sort",         required_argument, NULL, OPT_SORT},
    {"sortr",        required_argument, NULL, OPT_SORTR},
    {"sort-files",   no_argument,       NULL, OPT_SORT_FILES},
    {"no-sort-files", no_argument,      NULL, OPT_NO_SORT_FILES},
    {"engine",       required_argument, NULL, OPT_ENGINE},
    {"auto-hybrid-regex", no_argument,  NULL, OPT_AUTO_HYBRID_REGEX},
    {"no-auto-hybrid-regex", no_argument, NULL, OPT_NO_AUTO_HYBRID_REGEX},
    {"regex-size-limit", required_argument, NULL, OPT_REGEX_SIZE_LIMIT},
    {"dfa-size-limit", required_argument, NULL, OPT_DFA_SIZE_LIMIT},
    {"encoding",     required_argument, NULL, OPT_ENCODING},
    {"no-encoding",  no_argument,       NULL, OPT_NO_ENCODING},
    {"pcre2-version", no_argument,      NULL, OPT_PCRE2_VERSION},
    {"threads",      required_argument, NULL, 'j'},
    {"line-buffered", no_argument,      NULL, OPT_LINE_BUFFERED},
    {"no-line-buffered", no_argument,   NULL, OPT_NO_LINE_BUFFERED},
    {"block-buffered", no_argument,     NULL, OPT_BLOCK_BUFFERED},
    {"no-block-buffered", no_argument,  NULL, OPT_NO_BLOCK_BUFFERED},
    {"mmap",         no_argument,       NULL, OPT_MMAP},
    {"no-mmap",      no_argument,       NULL, OPT_NO_MMAP},
    {"include-zero", no_argument,       NULL, OPT_INCLUDE_ZERO},
    {"no-include-zero", no_argument,    NULL, OPT_NO_INCLUDE_ZERO},
    {"no-fixed-strings", no_argument,   NULL, OPT_NO_FIXED_STRINGS},
    {"no-follow",    no_argument,       NULL, OPT_NO_FOLLOW},
    {"no-invert-match", no_argument,    NULL, OPT_NO_INVERT_MATCH},
    {"no-stats",     no_argument,       NULL, OPT_NO_STATS},
    {"pretty",       no_argument,       NULL, OPT_PRETTY},
    {"one-file-system", no_argument,    NULL, OPT_ONE_FILE_SYSTEM},
    {"no-one-file-system", no_argument, NULL, OPT_NO_ONE_FILE_SYSTEM},
    {"path-separator", required_argument, NULL, OPT_PATH_SEPARATOR},
    {"pre",          required_argument, NULL, OPT_PRE},
    {"no-pre",       no_argument,       NULL, OPT_NO_PRE},
    {"pre-glob",     required_argument, NULL, OPT_PRE_GLOB},
    {"search-zip",   no_argument,       NULL, OPT_SEARCH_ZIP},
    {"no-search-zip", no_argument,      NULL, OPT_NO_SEARCH_ZIP},
    {"crlf",         no_argument,       NULL, OPT_CRLF},
    {"no-crlf",      no_argument,       NULL, OPT_NO_CRLF},
    {"hostname-bin", required_argument, NULL, OPT_HOSTNAME_BIN},
    {"hyperlink-format", required_argument, NULL, OPT_HYPERLINK_FORMAT},
    {"trim",         no_argument,       NULL, OPT_TRIM},
    {"no-trim",      no_argument,       NULL, OPT_NO_TRIM},
    {"unicode",      no_argument,       NULL, OPT_UNICODE},
    {"no-unicode",   no_argument,       NULL, OPT_NO_UNICODE},
    {"vimgrep",      no_argument,       NULL, OPT_VIMGREP},
    {"generate",     required_argument, NULL, OPT_GENERATE},
    {"no-config",    no_argument,       NULL, OPT_NO_CONFIG},
    {"unrestricted", no_argument,       NULL, 'u'},
    {NULL, 0, NULL, 0},
};

int bx_search_parse_options(int argc, char **argv, struct search_opts *opts,
                             enum bx_search_personality personality,
                             const char **pattern, int *first_file) {
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

    const struct option *selected_long_opts =
        bx_search_personality_is_rg(personality) ? long_opts : grep_long_opts;

    bx_args_getopt_reset();
    const char *short_opts = bx_search_personality_is_rg(personality)
                                 ? ":0.E:FHbhinovclLqr:RIszZd:M:aA:B:C:e:f:g:j:t:T:uwPxSm:UVNp"
                                 : ":0.EFGFHbhinovclLqrRIszZD:d:M:aA:B:C:e:f:g:j:t:uwPxSm:UVNpT";

    int c;
    while ((c = bx_args_getopt_long(argc, argv, short_opts, selected_long_opts, NULL)) != -1) {
        switch (c) {
        case ':':
            if (bx_search_personality_is_grep_family(personality)) {
                if (optind > 0 && optind <= argc
                    && bx_grep_unsupported_required_long_option(argv[optind - 1])) {
                    return bx_grep_unrecognized_option(progname, argv[optind - 1]);
                }
                return bx_grep_missing_option_argument(progname, optopt, optind, argc, argv);
            }
            if (optopt != 0) {
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", progname, optopt);
            } else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                fprintf(stderr, "%s: option requires an argument -- '%s'\n", progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: option requires an argument\n", progname);
            }
            return -1;
        case '0':
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "-0") != 0)
                return -1;
            opts->null_output = true;
            break;
        case '.':
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "-.") != 0)
                return -1;
            opts->hidden = true;
            break;
        case 'E':
            if (bx_search_personality_is_rg(personality)) {
                const char *token =
                    bx_search_current_option_token(optind, argc, argv, "-E");
                if (token && strcmp(token, "--extended-regexp") == 0)
                    return bx_grep_unrecognized_option(progname, token);
                if (!bx_rg_parse_encoding_name(progname, optarg, &opts->encoding_mode,
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-byte-offset") != 0)
                return -1;
            break;
        case OPT_COLUMN:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--column") != 0)
                return -1;
            opts->show_column = true;
            break;
        case OPT_NO_COLUMN:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-column") != 0)
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "-N") != 0)
                return -1;
            opts->show_line_number = false;
            break;
        case OPT_NO_LINE_NUMBER:
            opts->show_line_number = false;
            break;
        case 'o': opts->only_matching = true; break;
        case 'v': opts->invert_match = true; break;
        case OPT_NO_INVERT_MATCH:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-invert-match") != 0)
                return -1;
            opts->invert_match = false;
            break;
        case 'c': opts->count_only = true; break;
        case OPT_COUNT_MATCHES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--count-matches") != 0)
                return -1;
            opts->count_matches = true;
            opts->count_only = true;
            break;
        case OPT_PASSTHRU:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--passthru") != 0)
                return -1;
            opts->passthru = true;
            break;
        case OPT_REPLACE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--replace") != 0)
                return -1;
            free(opts->replace);
            opts->replace = strdup(optarg);
            if (!opts->replace)
                return -1;
            break;
        case OPT_STATS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--stats") != 0)
                return -1;
            opts->stats = true;
            break;
        case OPT_NO_STATS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-stats") != 0)
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
                    progname, personality, optind, argc, argv,
                    c == OPT_PRETTY ? "--pretty" : "-p") != 0) {
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
                const char *token =
                    bx_search_current_option_token(optind, argc, argv, "-r");
                if (token && strcmp(token, "--recursive") == 0)
                    return bx_grep_unrecognized_option(progname, token);
                free(opts->replace);
                opts->replace = strdup(optarg);
                if (!opts->replace)
                    return -1;
            } else {
                opts->recursive = true;
                opts->follow_symlinks = false;
            }
            break;
        case 'R':
            if (bx_search_personality_is_rg(personality)) {
                const char *token =
                    bx_search_current_option_token(optind, argc, argv, "-R");
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
                    && bx_search_current_option_is_long(optind, argc, argv, "--directories")) {
                    fprintf(stderr, "%s: unrecognized flag --directories\n", progname);
                    return -1;
                }
                if (!bx_parse_nonnegative_int(progname, "-d", optarg, &opts->max_depth))
                    return -1;
            } else {
                if (strcmp(optarg, "read") == 0) {
                    opts->directory_mode = BX_GREP_DIR_READ;
                } else if (strcmp(optarg, "recurse") == 0) {
                    opts->directory_mode = BX_GREP_DIR_RECURSE;
                    opts->recursive = true;
                    opts->follow_symlinks = false;
                } else if (strcmp(optarg, "skip") == 0) {
                    opts->directory_mode = BX_GREP_DIR_SKIP;
                } else {
                    if (bx_search_personality_is_grep_family(personality)) {
                        return bx_grep_invalid_directories_mode(progname, optarg);
                    }
                    fprintf(stderr, "%s: invalid argument for -d: %s\n", progname, optarg);
                    return -1;
                }
            }
            break;
        case 'D':
        case OPT_DEVICES:
            if (bx_search_personality_is_rg(personality)) {
                const char *fallback = c == OPT_DEVICES ? "--devices" : "-D";
                if (bx_search_current_option_is_long(optind, argc, argv, "--devices")) {
                    fprintf(stderr, "%s: unrecognized flag --devices\n", progname);
                    return -1;
                }
                return bx_search_require_rg_option(progname, personality, optind, argc,
                                                   argv, fallback);
            }
            if (strcmp(optarg, "read") == 0) {
                opts->device_mode = BX_GREP_DEVICE_READ;
            } else if (strcmp(optarg, "skip") == 0) {
                opts->device_mode = BX_GREP_DEVICE_SKIP;
            } else {
                fprintf(stderr, "%s: invalid argument '%s' for '--devices'\n",
                        progname, optarg);
                fputs("Valid arguments are:\n", stderr);
                fputs("  - 'read'\n", stderr);
                fputs("  - 'skip'\n", stderr);
                bx_grep_print_usage_try_help(progname);
                return 3;
            }
            opts->device_mode_explicit = true;
            break;
        case OPT_MAX_COLUMNS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--max-columns") != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "--max-columns", optarg, &opts->max_columns))
                return -1;
            break;
        case 'M':
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "-M") != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "-M", optarg, &opts->max_columns))
                return -1;
            break;
        case OPT_MAX_COLUMNS_PREVIEW:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--max-columns-preview") != 0)
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-text") != 0)
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-binary") != 0)
                return -1;
            opts->binary_as_text = false;
            opts->binary_without_match = true;
            break;
        case 'w': opts->word_regexp = true; break;
        case 'x': opts->line_regexp = true; break;
        case 'P':
            if (bx_search_personality_is_rg(personality)) {
                const char *token =
                    bx_search_current_option_token(optind, argc, argv, "-P");
                if (token && strcmp(token, "--perl-regexp") == 0)
                    return bx_grep_unrecognized_option(progname, token);
            }
            opts->perl_regexp = true;
            opts->extended_regex = false;
            opts->fixed_strings = false;
            break;
        case OPT_NO_PCRE2:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-pcre2") != 0)
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
                const char *token =
                    bx_search_current_option_token(optind, argc, argv, "-s");
                if (token && strncmp(token, "--case-sensitive", 16) == 0)
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
            if (!bx_search_parse_nonnegative_int(progname, personality, "-m", optarg, &opts->max_count))
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
            if (bx_search_personality_is_rg(personality) && optarg && optarg[0] == '!') {
                if (opts->num_exclude < MAX_EXCLUDE_PATTERNS) {
                    opts->exclude_patterns[opts->num_exclude] = strdup(optarg + 1);
                    opts->exclude_pattern_casefold[opts->num_exclude] =
                        opts->glob_case_insensitive;
                    opts->exclude_pattern_is_type[opts->num_exclude] = false;
                    opts->num_exclude++;
                }
            } else if (opts->num_include < MAX_INCLUDE_PATTERNS) {
                opts->include_patterns[opts->num_include] = strdup(optarg);
                opts->include_pattern_casefold[opts->num_include] =
                    opts->glob_case_insensitive;
                opts->include_pattern_is_type[opts->num_include] = false;
                opts->num_include++;
            }
            break;
        case OPT_IGLOB:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--iglob") != 0)
                return -1;
            if (optarg && optarg[0] == '!') {
                fprintf(stderr, "%s: unsupported option argument for --iglob: %s\n", progname, optarg);
                return -1;
            }
                if (opts->num_include < MAX_INCLUDE_PATTERNS) {
                    opts->include_patterns[opts->num_include] = strdup(optarg);
                    opts->include_pattern_casefold[opts->num_include] = true;
                    opts->include_pattern_is_type[opts->num_include] = false;
                    opts->num_include++;
                }
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-multiline") != 0)
                return -1;
            opts->multiline = false;
            break;
        case OPT_HIDDEN:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--hidden") != 0)
                return -1;
            opts->hidden = true;
            break;
        case OPT_NO_HIDDEN:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-hidden") != 0)
                return -1;
            opts->hidden = false;
            break;
        case 'j':
            if (!bx_thread_count_parse(progname, "--threads", optarg, &opts->threads))
                return -1;
            break;
        case 't':
        case 'T': {
            const char *arg = bx_search_current_option_token(optind, argc, argv, NULL);
            if (bx_search_personality_is_rg(personality) && c == 'T'
                && arg && strcmp(arg, "--initial-tab") == 0) {
                return bx_grep_unrecognized_option(progname, arg);
            }
            if (!bx_search_personality_is_rg(personality)) {
                if (c == 'T' && arg
                    && (strcmp(arg, "-T") == 0
                        || strcmp(arg, "--initial-tab") == 0)) {
                    opts->initial_tab = true;
                    break;
                }
                if (arg && (strcmp(arg, "--type") == 0 || strcmp(arg, "--type-not") == 0
                            || strncmp(arg, "--type=", 7) == 0
                            || strncmp(arg, "--type-not=", 11) == 0)) {
                    return bx_grep_unrecognized_option(progname, arg);
                }
                if (c == 't') {
                    fprintf(stderr, "%s: invalid option -- 't'\n", progname);
                    bx_grep_print_usage_try_help(progname);
                    return -1;
                }
                if (c == 'T') {
                    fprintf(stderr, "%s: invalid option -- 'T'\n", progname);
                    bx_grep_print_usage_try_help(progname);
                    return -1;
                }
            }
            const char *globs = bx_get_type_globs(opts, optarg);
            if (!globs) {
                if (bx_search_personality_is_rg(personality)) {
                    fprintf(stderr, "%s: unrecognized file type: %s\n", progname, optarg);
                    return -1;
                }
                break;
            }
            char *copy = strdup(globs);
            char *tok = strtok(copy, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                char *end = tok + strlen(tok) - 1;
                while (end > tok && *end == ' ') *end-- = '\0';
                if (c == 't' && opts->num_include < MAX_INCLUDE_PATTERNS) {
                    opts->include_patterns[opts->num_include] = strdup(tok);
                    opts->include_pattern_casefold[opts->num_include] =
                        opts->glob_case_insensitive;
                    opts->include_pattern_is_type[opts->num_include] = true;
                    opts->num_include++;
                } else if (c == 'T' && opts->num_exclude < MAX_EXCLUDE_PATTERNS) {
                    opts->exclude_patterns[opts->num_exclude] = strdup(tok);
                    opts->exclude_pattern_casefold[opts->num_exclude] =
                        opts->glob_case_insensitive;
                    opts->exclude_pattern_is_type[opts->num_exclude] = true;
                    opts->num_exclude++;
                }
                tok = strtok(NULL, ",");
            }
            free(copy);
            break;
        }
        case OPT_TYPE_ADD:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--type-add") != 0)
                return -1;
            if (!bx_add_custom_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-add: %s\n", progname, optarg);
                return -1;
            }
            break;
        case OPT_TYPE_CLEAR:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--type-clear") != 0)
                return -1;
            if (!bx_clear_type(opts, optarg)) {
                fprintf(stderr, "%s: invalid argument for --type-clear: %s\n", progname, optarg);
                return -1;
            }
            break;
        case 'e':
            if (opts->num_extra_patterns < 16) {
                char *copy = strdup(optarg);
                if (!copy)
                    return bx_search_parse_out_of_memory(progname);
                opts->extra_patterns[opts->num_extra_patterns++] = copy;
            }
            break;
        case 'f': {
            FILE *pf = NULL;
            bool close_pf = false;
            int read_errno = 0;
            struct bx_record_stream pattern_stream = {0};
            if (strcmp(optarg, "-") == 0) {
                pf = stdin;
            } else {
                pf = fopen(optarg, "r");
                close_pf = true;
            }
            if (!pf) {
                fprintf(stderr, "%s: %s: %s\n", progname, optarg, strerror(errno));
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
                if (opts->num_extra_patterns < 16) {
                    char *copy = strdup(line);
                    if (!copy) {
                        read_errno = ENOMEM;
                        break;
                    }
                    opts->extra_patterns[opts->num_extra_patterns++] = copy;
                }
            }
            if (close_pf && fclose(pf) != 0 && read_errno == 0)
                read_errno = errno != 0 ? errno : EIO;
            bx_record_stream_dispose(&pattern_stream);
            if (read_errno != 0)
                return bx_search_parse_path_error(progname, optarg, read_errno);
            break;
        }
        case 'A':
            if (!bx_parse_nonnegative_int(progname, "-A", optarg, &opts->after_context))
                return -1;
            break;
        case 'B':
            if (!bx_parse_nonnegative_int(progname, "-B", optarg, &opts->before_context))
                return -1;
            break;
        case 'C': {
            int n;
            if (!bx_parse_nonnegative_int(progname, "-C", optarg, &n))
                return -1;
            opts->after_context = n;
            opts->before_context = n;
            break;
        }
        case OPT_INCLUDE:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--include");
            if (opts->num_include < MAX_INCLUDE_PATTERNS)
                opts->include_patterns[opts->num_include++] = strdup(optarg);
            break;
        case OPT_EXCLUDE:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude");
            if (opts->num_exclude < MAX_EXCLUDE_PATTERNS)
                opts->exclude_patterns[opts->num_exclude++] = strdup(optarg);
            break;
        case OPT_EXCLUDE_FROM: {
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude-from");
            FILE *ef = fopen(optarg, "r");
            int read_errno = 0;
            struct bx_record_stream exclude_stream = {0};
            if (!ef) {
                fprintf(stderr, "%s: %s: %s\n", progname, optarg, strerror(errno));
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
                if (opts->num_exclude < MAX_EXCLUDE_PATTERNS) {
                    char *copy = strdup(line);
                    if (!copy) {
                        read_errno = ENOMEM;
                        break;
                    }
                    opts->exclude_patterns[opts->num_exclude++] = copy;
                }
            }
            if (fclose(ef) != 0 && read_errno == 0)
                read_errno = errno != 0 ? errno : EIO;
            bx_record_stream_dispose(&exclude_stream);
            if (read_errno != 0)
                return bx_search_parse_path_error(progname, optarg, read_errno);
            break;
        }
        case OPT_EXCLUDE_DIR:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--exclude-dir");
            if (opts->num_exclude_dir < MAX_EXCLUDE_DIR_PATTERNS)
                opts->exclude_dir_patterns[opts->num_exclude_dir++] = strdup(optarg);
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--follow") != 0)
                return -1;
            opts->follow_symlinks = true;
            break;
        case OPT_NO_FOLLOW:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-follow") != 0)
                return -1;
            opts->follow_symlinks = false;
            break;
        case OPT_IGNORE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore") != 0)
                return -1;
            opts->no_ignore = false;
            break;
        case OPT_IGNORE_PARENT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-parent") != 0)
                return -1;
            opts->no_ignore_parent = false;
            break;
        case OPT_IGNORE_VCS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-vcs") != 0)
                return -1;
            opts->no_ignore_vcs = false;
            break;
        case OPT_IGNORE_DOT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-dot") != 0)
                return -1;
            opts->no_ignore_dot = false;
            break;
        case OPT_IGNORE_EXCLUDE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-exclude") != 0)
                return -1;
            opts->no_ignore_exclude = false;
            break;
        case OPT_IGNORE_FILES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-files") != 0)
                return -1;
            opts->no_ignore_files = false;
            break;
        case OPT_IGNORE_GLOBAL:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-global") != 0)
                return -1;
            opts->no_ignore_global = false;
            break;
        case OPT_IGNORE_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-messages") != 0)
                return -1;
            opts->suppress_ignore_messages = false;
            break;
        case OPT_IGNORE_FILE_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-file-case-insensitive") != 0)
                return -1;
            opts->ignore_file_case_insensitive = true;
            break;
        case OPT_IGNORE_FILE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--ignore-file") != 0)
                return -1;
            if (!opts->no_ignore_files && opts->num_ignore_files < MAX_RG_IGNORE_FILES)
                opts->ignore_files[opts->num_ignore_files++] = strdup(optarg);
            break;
        case OPT_GLOB_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--glob-case-insensitive") != 0)
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-parent") != 0)
                return -1;
            opts->no_ignore_parent = true;
            break;
        case OPT_NO_IGNORE_VCS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-vcs") != 0)
                return -1;
            opts->no_ignore_vcs = true;
            break;
        case OPT_NO_IGNORE_DOT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-dot") != 0)
                return -1;
            opts->no_ignore_dot = true;
            break;
        case OPT_NO_IGNORE_EXCLUDE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-exclude") != 0)
                return -1;
            opts->no_ignore_exclude = true;
            break;
        case OPT_NO_IGNORE_FILES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-files") != 0)
                return -1;
            opts->no_ignore_files = true;
            break;
        case OPT_NO_IGNORE_GLOBAL:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-global") != 0)
                return -1;
            opts->no_ignore_global = true;
            break;
        case OPT_NO_IGNORE_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-messages") != 0)
                return -1;
            opts->suppress_ignore_messages = true;
            break;
        case OPT_NO_IGNORE_FILE_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-ignore-file-case-insensitive") != 0)
                return -1;
            opts->ignore_file_case_insensitive = false;
            break;
        case OPT_NO_GLOB_CASE_INSENSITIVE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-glob-case-insensitive") != 0)
                return -1;
            opts->glob_case_insensitive = false;
            break;
        case OPT_NO_REQUIRE_GIT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-require-git") != 0)
                return -1;
            opts->no_require_git = true;
            break;
        case OPT_REQUIRE_GIT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--require-git") != 0)
                return -1;
            opts->no_require_git = false;
            break;
        case OPT_TYPE_LIST:
            bx_search_print_type_list();
            return 1;
        case OPT_PRINT0:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--print0") != 0)
                return -1;
            opts->null_output = true;
            break;
        case OPT_MAX_DEPTH:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--max-depth") != 0)
                return -1;
            if (!bx_parse_nonnegative_int(progname, "--max-depth", optarg, &opts->max_depth))
                return -1;
            break;
        case OPT_LABEL:
            if (bx_search_personality_is_rg(personality))
                return bx_grep_unrecognized_option(progname, "--label");
            free(opts->label);
            opts->label = strdup(optarg);
            break;
        case OPT_GROUP_SEPARATOR:
            if (bx_search_reject_rg_option(progname, personality, optind, argc,
                                           argv, "--group-separator") != 0) {
                return -1;
            }
            free(opts->group_separator);
            opts->group_separator = strdup(optarg);
            opts->suppress_group_separator = false;
            break;
        case OPT_NO_GROUP_SEPARATOR:
            if (bx_search_reject_rg_option(progname, personality, optind, argc,
                                           argv, "--no-group-separator") != 0) {
                return -1;
            }
            opts->suppress_group_separator = true;
            break;
        case OPT_CONTEXT_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--context-separator") != 0)
                return -1;
            free(opts->group_separator);
            opts->group_separator = strdup(optarg);
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
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--field-context-separator") != 0)
                return -1;
            free(opts->field_context_separator);
            opts->field_context_separator = strdup(optarg);
            break;
        case OPT_FIELD_MATCH_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--field-match-separator") != 0)
                return -1;
            free(opts->field_match_separator);
            opts->field_match_separator = strdup(optarg);
            break;
        case OPT_HEADING:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--heading") != 0)
                return -1;
            opts->heading = true;
            opts->heading_set = true;
            break;
        case OPT_NO_HEADING:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-heading") != 0)
                return -1;
            opts->heading = false;
            opts->heading_set = true;
            break;
        case OPT_NULL:
            opts->null_output = true;
            opts->null_filename = true;
            break;
        case OPT_BINARY_FILES:
            if (bx_search_reject_rg_option(progname, personality, optind, argc,
                                           argv, "--binary-files") != 0) {
                return -1;
            }
            if (!bx_set_binary_files_mode(progname, personality, opts, optarg))
                return -1;
            break;
        case OPT_NULL_DATA:
            opts->null_data = true;
            break;
        case OPT_NO_MESSAGES:
            opts->suppress_errors = true;
            break;
        case OPT_MESSAGES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--messages") != 0)
                return -1;
            opts->suppress_errors = false;
            break;
        case OPT_MULTILINE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--multiline") != 0)
                return -1;
            opts->multiline = true;
            break;
        case OPT_MULTILINE_DOTALL:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--multiline-dotall") != 0)
                return -1;
            opts->multiline = true;
            opts->multiline_dotall = true;
            break;
        case OPT_NO_MULTILINE_DOTALL:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-multiline-dotall") != 0)
                return -1;
            opts->multiline_dotall = false;
            break;
        case OPT_STOP_ON_NONMATCH:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--stop-on-nonmatch") != 0)
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
                progname, personality, optind, argc, argv, "--json");
        }
        case OPT_NO_JSON:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-json") != 0)
                return -1;
            break;
        case OPT_DEBUG:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--debug") != 0)
                return -1;
            fprintf(stderr, "%s: unsupported option '--debug'\n", progname);
            return -1;
        case OPT_TRACE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--trace") != 0)
                return -1;
            opts->trace = true;
            break;
        case OPT_SORT:
        case OPT_SORTR:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, c == OPT_SORT ? "--sort" : "--sortr") != 0)
                return -1;
            if (!bx_search_parse_sort_key(optarg, &opts->sort_key)) {
                fprintf(stderr,
                        "%s: unsupported argument for %s: %s\n",
                        progname,
                        c == OPT_SORT ? "--sort" : "--sortr",
                        optarg);
                return -1;
            }
            opts->sort_dir = c == OPT_SORTR
                                 ? BX_SEARCH_SORT_DESCENDING
                                 : BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_SORT_FILES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--sort-files") != 0)
                return -1;
            opts->sort_key = BX_SEARCH_SORT_PATH;
            opts->sort_dir = BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_NO_SORT_FILES:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-sort-files") != 0)
                return -1;
            opts->sort_key = BX_SEARCH_SORT_NONE;
            opts->sort_dir = BX_SEARCH_SORT_ASCENDING;
            break;
        case OPT_ENGINE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--engine") != 0)
                return -1;
            if (strcmp(optarg, "default") == 0)
                opts->rg_engine = BX_RG_ENGINE_DEFAULT;
            else if (strcmp(optarg, "pcre2") == 0)
                opts->rg_engine = BX_RG_ENGINE_PCRE2;
            else if (strcmp(optarg, "auto") == 0)
                opts->rg_engine = BX_RG_ENGINE_AUTO;
            else {
                fprintf(stderr,
                        "%s: error parsing flag --engine: unrecognized regex engine '%s'\n",
                        progname, optarg);
                return -1;
            }
            break;
        case OPT_AUTO_HYBRID_REGEX:
            return bx_search_unsupported_rg_option(
                progname, personality, optind, argc, argv, "--auto-hybrid-regex");
        case OPT_NO_AUTO_HYBRID_REGEX:
            return bx_search_unsupported_rg_option(
                progname, personality, optind, argc, argv, "--no-auto-hybrid-regex");
        case OPT_PCRE2_VERSION:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--pcre2-version") != 0)
                return -1;
            opts->pcre2_version = true;
            bx_regex_print_version();
            return 1;
        case OPT_REGEX_SIZE_LIMIT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--regex-size-limit") != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--regex-size-limit", optarg,
                                        &opts->regex_size_limit))
                return -1;
            opts->regex_size_limit_set = true;
            break;
        case OPT_DFA_SIZE_LIMIT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--dfa-size-limit") != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--dfa-size-limit", optarg,
                                        &opts->dfa_size_limit))
                return -1;
            opts->dfa_size_limit_set = true;
            break;
        case OPT_ENCODING:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--encoding") != 0)
                return -1;
            if (!bx_rg_parse_encoding_name(progname, optarg, &opts->encoding_mode,
                                           &opts->encoding_name))
                return -1;
            break;
        case OPT_NO_ENCODING:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-encoding") != 0)
                return -1;
            free(opts->encoding_name);
            opts->encoding_name = NULL;
            opts->encoding_mode = BX_RG_ENCODING_AUTO;
            break;
        case OPT_COLOR:
            if (bx_search_personality_is_rg(personality)
                && bx_search_current_option_is_long(optind, argc, argv, "--colour")) {
                fprintf(stderr, "%s: unrecognized flag --colour\n", progname);
                return -1;
            }
            opts->color_mode = bx_color_parse(optarg ? optarg : "auto");
            bx_color_set_mode(opts->color_mode);
            break;
        case OPT_COLORS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--colors") != 0)
                return -1;
            if (!bx_rg_parse_colors_spec(progname, optarg, &opts->rg_colors))
                return -1;
            break;
        case OPT_CRLF:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--crlf") != 0)
                return -1;
            opts->crlf = true;
            opts->null_data = false;
            break;
        case OPT_NO_CRLF:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-crlf") != 0)
                return -1;
            opts->crlf = false;
            break;
        case OPT_HOSTNAME_BIN:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--hostname-bin") != 0)
                return -1;
            free(opts->hostname_bin);
            opts->hostname_bin = NULL;
            if (optarg[0] != '\0') {
                opts->hostname_bin = strdup(optarg);
                if (!opts->hostname_bin)
                    return -1;
            }
            break;
        case OPT_HYPERLINK_FORMAT:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--hyperlink-format") != 0)
                return -1;
            if (!bx_rg_parse_hyperlink_format(progname, optarg,
                                              &opts->hyperlink_format)) {
                return -1;
            }
            break;
        case OPT_NO_MAX_COLUMNS_PREVIEW:
            if (bx_search_require_rg_option(
                    progname, personality, optind, argc, argv,
                    bx_search_current_option_token(optind, argc, argv, NULL)) != 0) {
                return -1;
            }
            break;
        case OPT_NO_PCRE2_UNICODE:
            return bx_search_unsupported_rg_option(
                progname, personality, optind, argc, argv, "--no-pcre2-unicode");
        case OPT_PCRE2_UNICODE:
            return bx_search_unsupported_rg_option(
                progname, personality, optind, argc, argv, "--pcre2-unicode");
        case OPT_PATH_SEPARATOR:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--path-separator") != 0)
                return -1;
            if (!bx_rg_parse_path_separator(progname, optarg,
                                            &opts->path_separator)) {
                return -1;
            }
            break;
        case OPT_PRE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--pre") != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            if (optarg[0] != '\0') {
                opts->pre_command = strdup(optarg);
                if (!opts->pre_command)
                    return -1;
                opts->search_zip = false;
            }
            break;
        case OPT_NO_PRE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-pre") != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            break;
        case OPT_PRE_GLOB:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--pre-glob") != 0)
                return -1;
            if (opts->num_pre_globs < MAX_PRE_GLOBS) {
                opts->pre_globs[opts->num_pre_globs] = strdup(optarg);
                if (!opts->pre_globs[opts->num_pre_globs])
                    return -1;
                opts->num_pre_globs++;
            }
            break;
        case OPT_SEARCH_ZIP:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--search-zip") != 0)
                return -1;
            free(opts->pre_command);
            opts->pre_command = NULL;
            opts->search_zip = true;
            break;
        case OPT_NO_SEARCH_ZIP:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-search-zip") != 0)
                return -1;
            opts->search_zip = false;
            break;
        case OPT_TRIM:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--trim") != 0)
                return -1;
            opts->trim = true;
            break;
        case OPT_NO_TRIM:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-trim") != 0)
                return -1;
            opts->trim = false;
            break;
        case OPT_UNICODE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--unicode") != 0)
                return -1;
            opts->unicode = true;
            break;
        case OPT_NO_UNICODE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-unicode") != 0)
                return -1;
            opts->unicode = false;
            break;
        case OPT_LINE_BUFFERED:
            opts->line_buffered = true;
            break;
        case OPT_NO_LINE_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-line-buffered") != 0)
                return -1;
            opts->line_buffered = false;
            break;
        case OPT_BLOCK_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--block-buffered") != 0)
                return -1;
            break;
        case OPT_NO_BLOCK_BUFFERED:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-block-buffered") != 0)
                return -1;
            break;
        case OPT_MMAP:
        case OPT_NO_MMAP:
            if (bx_search_require_rg_option(
                    progname, personality, optind, argc, argv,
                    bx_search_current_option_token(optind, argc, argv, NULL)) != 0) {
                return -1;
            }
            break;
        case OPT_INCLUDE_ZERO:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--include-zero") != 0)
                return -1;
            opts->include_zero = true;
            break;
        case OPT_NO_INCLUDE_ZERO:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-include-zero") != 0)
                return -1;
            opts->include_zero = false;
            break;
        case OPT_NO_FIXED_STRINGS:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-fixed-strings") != 0)
                return -1;
            opts->fixed_strings = false;
            break;
        case OPT_ONE_FILE_SYSTEM:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--one-file-system") != 0)
                return -1;
            opts->stay_on_filesystem = true;
            break;
        case OPT_NO_ONE_FILE_SYSTEM:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-one-file-system") != 0)
                return -1;
            opts->stay_on_filesystem = false;
            break;
        case OPT_MAX_FILESIZE:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--max-filesize") != 0)
                return -1;
            if (!bx_parse_rg_size_limit(progname, "--max-filesize", optarg,
                                        &opts->max_filesize))
                return -1;
            opts->max_filesize_set = true;
            break;
        case OPT_VIMGREP:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--vimgrep") != 0)
                return -1;
            opts->vimgrep = true;
            break;
        case OPT_GENERATE:
            return bx_search_unsupported_rg_option(
                progname, personality, optind, argc, argv, "--generate");
        case OPT_NO_CONFIG:
            if (bx_search_require_rg_option(progname, personality, optind, argc,
                                            argv, "--no-config") != 0)
                return -1;
            break;
        case OPT_HELP:
            bx_search_print_help(progname);
            return 1;
        case 'V':
        case OPT_VERSION:
            bx_search_print_version(progname);
            return 1;
        case '?':
            if (!bx_search_personality_is_rg(personality) && optind > 0 && optind <= argc) {
                const char *arg = argv[optind - 1];
                if (arg && arg[0] == '-' && isdigit((unsigned char)arg[1])) {
                    bool all_digits = true;
                    for (const char *p = arg + 1; *p; p++) {
                        if (!isdigit((unsigned char)*p)) {
                            all_digits = false;
                            break;
                        }
                    }
                    if (all_digits) {
                        int n;
                        if (!bx_parse_nonnegative_int(progname, arg, arg + 1, &n))
                            return -1;
                        opts->after_context = n;
                        opts->before_context = n;
                        break;
                    }
                }
            }
            if (optopt) {
                fprintf(stderr, "%s: invalid option -- '%c'\n", progname, optopt);
            } else if (optind > 0 && optind <= argc) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", progname, argv[optind - 1]);
            } else {
                fprintf(stderr, "%s: unrecognized option\n", progname);
            }
            if (personality == BX_SEARCH_GREP) {
                bx_grep_print_usage_try_help(progname);
            }
            return -1;
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

    if (opts->files_only) {
        *pattern = "";
        *first_file = optind;
        return 0;
    }

        if (optind >= argc) {
            if (!opts->files_only && opts->num_extra_patterns == 0) {
                if (personality == BX_SEARCH_GREP) {
                    bx_grep_print_usage_try_help(progname);
                } else {
                    fprintf(stderr,
                            "%s: ripgrep requires at least one pattern to execute a search\n",
                            progname);
                }
                return -1;
            }
        *pattern = opts->num_extra_patterns > 0 ? opts->extra_patterns[--opts->num_extra_patterns] : "";
    } else if (opts->num_extra_patterns > 0) {
        *pattern = opts->extra_patterns[--opts->num_extra_patterns];
    } else {
        *pattern = argv[optind++];
    }
    *first_file = optind;
    return 0;
}
