#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <grp.h>
#include <inttypes.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"

enum bx_ls_variant {
    BX_LS_VARIANT_LS,
    BX_LS_VARIANT_DIR,
    BX_LS_VARIANT_VDIR,
};

enum bx_ls_format {
    BX_LS_FORMAT_SINGLE,
    BX_LS_FORMAT_COLUMNS,
    BX_LS_FORMAT_LONG,
};

enum bx_ls_sort_mode {
    BX_LS_SORT_NAME = 0,
    BX_LS_SORT_TIME,
    BX_LS_SORT_SIZE,
};

enum bx_ls_option_code {
    BX_LS_OPT_HELP = 1,
    BX_LS_OPT_VERSION = 2,
    BX_LS_OPT_FORMAT = 3,
    BX_LS_OPT_COLOR = 4,
    BX_LS_OPT_SI = 5,
    BX_LS_OPT_AUTHOR = 6,
    BX_LS_OPT_BLOCK_SIZE = 7,
    BX_LS_OPT_FULL_TIME = 8,
    BX_LS_OPT_GROUP_DIRECTORIES_FIRST = 9,
    BX_LS_OPT_DEREFERENCE_CMDLINE_SYMLINK_TO_DIR = 10,
    BX_LS_OPT_HIDE = 11,
    BX_LS_OPT_HYPERLINK = 12,
    BX_LS_OPT_INDICATOR_STYLE = 13,
    BX_LS_OPT_SHOW_CONTROL_CHARS = 14,
    BX_LS_OPT_QUOTING_STYLE = 15,
    BX_LS_OPT_SORT = 16,
    BX_LS_OPT_TIME = 17,
    BX_LS_OPT_TIME_STYLE = 18,
    BX_LS_OPT_ZERO = 19,
};

enum bx_ls_color_when {
    BX_LS_COLOR_NEVER = 0,
    BX_LS_COLOR_ALWAYS,
    BX_LS_COLOR_AUTO,
};

struct bx_ls_options {
    const char* progname;
    enum bx_ls_variant variant;
    enum bx_ls_format format;
    bool show_all;
    bool almost_all;
    bool directory_mode;
    bool recursive;
    bool classify;
    bool slash_directories;
    bool show_inode;
    bool numeric_ids;
    bool human_readable;
    bool si_units;
    bool escape_names;
    bool sort_entries;
    bool reverse_sort;
    enum bx_ls_sort_mode sort_mode;
    bool show_help;
    bool show_version;
    enum bx_ls_color_when color_when;
};

struct bx_ls_entry {
    char* name;
    char* full_path;
    struct stat st;
    bool has_stat;
};

struct bx_ls_entry_list {
    struct bx_ls_entry* items;
    size_t len;
    size_t cap;
};

struct bx_ls_path_list {
    char** items;
    size_t len;
    size_t cap;
};

struct bx_ls_long_widths {
    size_t inode;
    size_t nlink;
    size_t user;
    size_t group;
    size_t size;
};

static const char* bx_ls_variant_name(enum bx_ls_variant variant) {
    switch (variant) {
        case BX_LS_VARIANT_DIR:
            return "dir";
        case BX_LS_VARIANT_VDIR:
            return "vdir";
        case BX_LS_VARIANT_LS:
        default:
            return "ls";
    }
}

static void bx_ls_print_help(FILE* stream, const struct bx_ls_options* options) {
    fprintf(stream, "Usage: %s [OPTION]... [FILE]...\n", options->progname);
    fputs("List information about the FILEs (the current directory by default).\n", stream);
    fputs("Sort entries alphabetically if none of -cftuvSUX nor --sort is specified.\n", stream);
    fputs("\n", stream);
    fputs("Mandatory arguments to long options are mandatory for short options too.\n", stream);
    fputs("  -a, --all\n", stream);
    fputs("         do not ignore entries starting with .\n", stream);
    fputs("  -A, --almost-all\n", stream);
    fputs("         do not list implied . and ..\n", stream);
    fputs("      --author\n", stream);
    fputs("         with -l, print the author of each file\n", stream);
    fputs("  -b, --escape\n", stream);
    fputs("         print C-style escapes for nongraphic characters\n", stream);
    fputs("      --block-size=SIZE\n", stream);
    fputs("         with -l, scale sizes by SIZE when printing them;\n", stream);
    fputs("         e.g., '--block-size=M'; see SIZE format below\n", stream);
    fputs("  -B, --ignore-backups\n", stream);
    fputs("         do not list implied entries ending with ~\n", stream);
    fputs("  -c\n", stream);
    fputs("         with -lt: sort by, and show, ctime\n", stream);
    fputs("           (time of last change of file status information);\n", stream);
    fputs("         with -l: show ctime and sort by name;\n", stream);
    fputs("         otherwise: sort by ctime, newest first\n", stream);
    fputs("  -C\n", stream);
    fputs("         list entries by columns\n", stream);
    fputs("      --color[=WHEN]\n", stream);
    fputs("         color the output WHEN; more info below\n", stream);
    fputs("  -d, --directory\n", stream);
    fputs("         list directories themselves, not their contents\n", stream);
    fputs("  -D, --dired\n", stream);
    fputs("         generate output designed for Emacs' dired mode\n", stream);
    fputs("  -f\n", stream);
    fputs("         same as -a -U\n", stream);
    fputs("  -F, --classify[=WHEN]\n", stream);
    fputs("         append indicator (one of */=>@|) to entries WHEN\n", stream);
    fputs("      --file-type\n", stream);
    fputs("         like -F, except do not append '*'\n", stream);
    fputs("      --format=WORD\n", stream);
    fputs("         across,horizontal (-x), commas (-m), long (-l),\n", stream);
    fputs("         single-column (-1), verbose (-l), vertical (-C)\n", stream);
    fputs("      --full-time\n", stream);
    fputs("         like -l --time-style=full-iso\n", stream);
    fputs("  -g\n", stream);
    fputs("         like -l, but do not list owner\n", stream);
    fputs("      --group-directories-first\n", stream);
    fputs("         group directories before files\n", stream);
    fputs("  -G, --no-group\n", stream);
    fputs("         in a long listing, don't print group names\n", stream);
    fputs("  -h, --human-readable\n", stream);
    fputs("         with -l and -s, print sizes like 1K 234M 2G etc.\n", stream);
    fputs("      --si\n", stream);
    fputs("         likewise, but use powers of 1000 not 1024\n", stream);
    fputs("  -H, --dereference-command-line\n", stream);
    fputs("         follow symbolic links listed on the command line\n", stream);
    fputs("      --dereference-command-line-symlink-to-dir\n", stream);
    fputs("         follow each command line symbolic link that points to a directory\n", stream);
    fputs("      --hide=PATTERN\n", stream);
    fputs("         do not list implied entries matching shell PATTERN\n", stream);
    fputs("         (overridden by -a or -A)\n", stream);
    fputs("      --hyperlink[=WHEN]\n", stream);
    fputs("         hyperlink file names WHEN\n", stream);
    fputs("      --indicator-style=WORD\n", stream);
    fputs("         append indicator with style WORD to entry names:\n", stream);
    fputs("           none (default), slash (-p), file-type (--file-type), classify (-F)\n", stream);
    fputs("  -i, --inode\n", stream);
    fputs("         print the index number of each file\n", stream);
    fputs("  -I, --ignore=PATTERN\n", stream);
    fputs("         do not list implied entries matching shell PATTERN\n", stream);
    fputs("  -k, --kibibytes\n", stream);
    fputs("         default to 1024-byte blocks for file system usage;\n", stream);
    fputs("         used only with -s and per directory totals\n", stream);
    fputs("  -l\n", stream);
    fputs("         use a long listing format\n", stream);
    fputs("  -L, --dereference\n", stream);
    fputs("         when showing file information for a symbolic link,\n", stream);
    fputs("         show information for the file the link references\n", stream);
    fputs("         rather than for the link itself\n", stream);
    fputs("  -m\n", stream);
    fputs("         fill width with a comma separated list of entries\n", stream);
    fputs("  -n, --numeric-uid-gid\n", stream);
    fputs("         like -l, but list numeric user and group IDs\n", stream);
    fputs("  -N, --literal\n", stream);
    fputs("         print entry names without quoting\n", stream);
    fputs("  -o\n", stream);
    fputs("         like -l, but do not list group information\n", stream);
    fputs("  -p, --indicator-style=slash\n", stream);
    fputs("         append / indicator to directories\n", stream);
    fputs("  -q, --hide-control-chars\n", stream);
    fputs("         print ? instead of nongraphic characters\n", stream);
    fputs("      --show-control-chars\n", stream);
    fputs("         show nongraphic characters as-is;\n", stream);
    fputs("         the default, unless program is 'ls' and output is a terminal\n", stream);
    fputs("  -Q, --quote-name\n", stream);
    fputs("         enclose entry names in double quotes\n", stream);
    fputs("      --quoting-style=WORD\n", stream);
    fputs("         use quoting style WORD for entry names:\n", stream);
    fputs("           literal, locale, shell, shell-always,\n", stream);
    fputs("           shell-escape, shell-escape-always, c, escape\n", stream);
    fputs("         (overrides QUOTING_STYLE environment variable)\n", stream);
    fputs("  -r, --reverse\n", stream);
    fputs("         reverse order while sorting\n", stream);
    fputs("  -R, --recursive\n", stream);
    fputs("         list subdirectories recursively\n", stream);
    fputs("  -s, --size\n", stream);
    fputs("         print the allocated size of each file, in blocks\n", stream);
    fputs("  -S\n", stream);
    fputs("         sort by file size, largest first\n", stream);
    fputs("      --sort=WORD\n", stream);
    fputs("         change default 'name' sort to WORD:\n", stream);
    fputs("           none (-U), size (-S), time (-t),\n", stream);
    fputs("           version (-v), extension (-X), name, width\n", stream);
    fputs("      --time=WORD\n", stream);
    fputs("         select which timestamp used to display or sort;\n", stream);
    fputs("           access time (-u): atime, access, use;\n", stream);
    fputs("           metadata change time (-c): ctime, status;\n", stream);
    fputs("           modified time (default): mtime, modification;\n", stream);
    fputs("           birth time: birth, creation;\n", stream);
    fputs("         with -l, WORD determines which time to show;\n", stream);
    fputs("         with --sort=time, sort by WORD (newest first)\n", stream);
    fputs("      --time-style=TIME_STYLE\n", stream);
    fputs("         time/date format with -l; see TIME_STYLE below\n", stream);
    fputs("  -t\n", stream);
    fputs("         sort by time, newest first; see --time\n", stream);
    fputs("  -T, --tabsize=COLS\n", stream);
    fputs("         assume tab stops at each COLS instead of 8\n", stream);
    fputs("  -u\n", stream);
    fputs("         with -lt: sort by, and show, access time;\n", stream);
    fputs("         with -l: show access time and sort by name;\n", stream);
    fputs("         otherwise: sort by access time, newest first\n", stream);
    fputs("  -U\n", stream);
    fputs("         do not sort directory entries\n", stream);
    fputs("  -v\n", stream);
    fputs("         natural sort of (version) numbers within text\n", stream);
    fputs("  -w, --width=COLS\n", stream);
    fputs("         set output width to COLS.  0 means no limit\n", stream);
    fputs("  -x\n", stream);
    fputs("         list entries by lines instead of by columns\n", stream);
    fputs("  -X\n", stream);
    fputs("         sort alphabetically by entry extension\n", stream);
    fputs("  -Z, --context\n", stream);
    fputs("         print any security context of each file\n", stream);
    fputs("      --zero\n", stream);
    fputs("         end each output line with NUL, not newline\n", stream);
    fputs("  -1\n", stream);
    fputs("         list one file per line\n", stream);
    fputs("      --help\n", stream);
    fputs("         display this help and exit\n", stream);
    fputs("      --version\n", stream);
    fputs("         output version information and exit\n", stream);
    fputs("\n", stream);
    fputs("The SIZE argument is an integer and optional unit (example: 10K is 10*1024).\n", stream);
    fputs("Units are K,M,G,T,P,E,Z,Y,R,Q (powers of 1024) or KB,MB,... (powers of 1000).\n", stream);
    fputs("Binary prefixes can be used, too: KiB=K, MiB=M, and so on.\n", stream);
    fputs("\n", stream);
    fputs("The TIME_STYLE argument can be full-iso, long-iso, iso, locale, or +FORMAT.\n", stream);
    fputs("FORMAT is interpreted like in date(1).  If FORMAT is FORMAT1<newline>FORMAT2,\n", stream);
    fputs("then FORMAT1 applies to non-recent files and FORMAT2 to recent files.\n", stream);
    fputs("TIME_STYLE prefixed with 'posix-' takes effect only outside the POSIX locale.\n", stream);
    fputs("Also the TIME_STYLE environment variable sets the default style to use.\n", stream);
    fputs("\n", stream);
    fputs("The WHEN argument defaults to 'always' and can also be 'auto' or 'never'.\n", stream);
    fputs("\n", stream);
    fputs("Using color to distinguish file types is disabled both by default and\n", stream);
    fputs("with --color=never.  With --color=auto, ls emits color codes only when\n", stream);
    fputs("standard output is connected to a terminal.  The LS_COLORS environment\n", stream);
    fputs("variable can change the settings.  Use the dircolors(1) command to set it.\n", stream);
    fputs("\n", stream);
    fputs("Exit status:\n", stream);
    fputs(" 0  if OK,\n", stream);
    fputs(" 1  if minor problems (e.g., cannot access subdirectory),\n", stream);
    fputs(" 2  if serious trouble (e.g., cannot access command-line argument).\n", stream);
}

static enum bx_ls_format bx_ls_default_format(enum bx_ls_variant variant) {
    switch (variant) {
        case BX_LS_VARIANT_DIR:
            return BX_LS_FORMAT_COLUMNS;
        case BX_LS_VARIANT_VDIR:
            return BX_LS_FORMAT_LONG;
        case BX_LS_VARIANT_LS:
        default:
            return isatty(STDOUT_FILENO) ? BX_LS_FORMAT_COLUMNS : BX_LS_FORMAT_SINGLE;
    }
}

static void bx_ls_options_init(struct bx_ls_options* options, enum bx_ls_variant variant, const char* argv0) {
    memset(options, 0, sizeof(*options));
    options->progname = bx_cli_progname(argv0, bx_ls_variant_name(variant));
    options->variant = variant;
    options->format = bx_ls_default_format(variant);
    options->sort_entries = true;
    options->sort_mode = BX_LS_SORT_NAME;
    options->escape_names = (variant != BX_LS_VARIANT_LS);
    options->color_when = BX_LS_COLOR_NEVER;
}

static bool bx_ls_parse_format_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--format' requires an argument");
        return false;
    }

    if (strcmp(text, "long") == 0 || strcmp(text, "verbose") == 0) {
        options->format = BX_LS_FORMAT_LONG;
        return true;
    }

    if (strcmp(text, "single-column") == 0) {
        options->format = BX_LS_FORMAT_SINGLE;
        return true;
    }

    if (strcmp(text, "commas") == 0) {
        options->format = BX_LS_FORMAT_SINGLE;
        return true;
    }

    if (strcmp(text, "across") == 0 || strcmp(text, "horizontal") == 0 || strcmp(text, "vertical") == 0) {
        options->format = BX_LS_FORMAT_COLUMNS;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--format'", text);
    return false;
}

static bool bx_ls_parse_color_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    const char* when = text;
    if (when == NULL) {
        when = "always";
    }

    if (strcmp(when, "always") == 0) {
        options->color_when = BX_LS_COLOR_ALWAYS;
        return true;
    }
    if (strcmp(when, "auto") == 0) {
        options->color_when = BX_LS_COLOR_AUTO;
        return true;
    }
    if (strcmp(when, "never") == 0) {
        options->color_when = BX_LS_COLOR_NEVER;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--color'", when);
    return false;
}

static bool bx_ls_parse_sort_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--sort' requires an argument");
        return false;
    }

    if (strcmp(text, "none") == 0) {
        options->sort_entries = false;
        return true;
    }

    if (strcmp(text, "name") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_NAME;
        return true;
    }

    if (strcmp(text, "time") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_TIME;
        return true;
    }

    if (strcmp(text, "size") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_SIZE;
        return true;
    }

    if (strcmp(text, "version") == 0 || strcmp(text, "extension") == 0 || strcmp(text, "width") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_NAME;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--sort'", text);
    return false;
}

static bool bx_ls_parse_options(int argc, char** argv, enum bx_ls_variant variant, struct bx_ls_options* options, int* first_operand, struct bx_diag_ctx* diag) {
    static const struct option long_options[] = {
        {"all", no_argument, NULL, 'a'},
        {"almost-all", no_argument, NULL, 'A'},
        {"author", no_argument, NULL, BX_LS_OPT_AUTHOR},
        {"block-size", required_argument, NULL, BX_LS_OPT_BLOCK_SIZE},
        {"ignore-backups", no_argument, NULL, 'B'},
        {"color", optional_argument, NULL, BX_LS_OPT_COLOR},
        {"directory", no_argument, NULL, 'd'},
        {"dired", no_argument, NULL, 'D'},
        {"classify", optional_argument, NULL, 'F'},
        {"file-type", no_argument, NULL, 'p'},
        {"full-time", no_argument, NULL, BX_LS_OPT_FULL_TIME},
        {"group-directories-first", no_argument, NULL, BX_LS_OPT_GROUP_DIRECTORIES_FIRST},
        {"no-group", no_argument, NULL, 'G'},
        {"dereference-command-line", no_argument, NULL, 'H'},
        {"dereference-command-line-symlink-to-dir", no_argument, NULL, BX_LS_OPT_DEREFERENCE_CMDLINE_SYMLINK_TO_DIR},
        {"hide", required_argument, NULL, BX_LS_OPT_HIDE},
        {"hyperlink", optional_argument, NULL, BX_LS_OPT_HYPERLINK},
        {"indicator-style", required_argument, NULL, BX_LS_OPT_INDICATOR_STYLE},
        {"inode", no_argument, NULL, 'i'},
        {"ignore", required_argument, NULL, 'I'},
        {"kibibytes", no_argument, NULL, 'k'},
        {"dereference", no_argument, NULL, 'L'},
        {"numeric-uid-gid", no_argument, NULL, 'n'},
        {"literal", no_argument, NULL, 'N'},
        {"hide-control-chars", no_argument, NULL, 'q'},
        {"show-control-chars", no_argument, NULL, BX_LS_OPT_SHOW_CONTROL_CHARS},
        {"quote-name", no_argument, NULL, 'Q'},
        {"quoting-style", required_argument, NULL, BX_LS_OPT_QUOTING_STYLE},
        {"reverse", no_argument, NULL, 'r'},
        {"size", no_argument, NULL, 's'},
        {"sort", required_argument, NULL, BX_LS_OPT_SORT},
        {"time", required_argument, NULL, BX_LS_OPT_TIME},
        {"time-style", required_argument, NULL, BX_LS_OPT_TIME_STYLE},
        {"tabsize", required_argument, NULL, 'T'},
        {"width", required_argument, NULL, 'w'},
        {"context", no_argument, NULL, 'Z'},
        {"zero", no_argument, NULL, BX_LS_OPT_ZERO},
        {"human-readable", no_argument, NULL, 'h'},
        {"si", no_argument, NULL, BX_LS_OPT_SI},
        {"escape", no_argument, NULL, 'b'},
        {"recursive", no_argument, NULL, 'R'},
        {"format", required_argument, NULL, BX_LS_OPT_FORMAT},
        {"help", no_argument, NULL, BX_LS_OPT_HELP},
        {"version", no_argument, NULL, BX_LS_OPT_VERSION},
        {NULL, 0, NULL, 0},
    };

    bx_ls_options_init(options, variant, (argc > 0) ? argv[0] : NULL);
    diag->progname = options->progname;

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+1ABCDfFGHI:kLNQRST:UXZabcdghilmnopqrstuvw:x", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '1':
                options->format = BX_LS_FORMAT_SINGLE;
                break;
            case 'A':
                options->almost_all = true;
                options->show_all = false;
                break;
            case 'B':
                break;
            case 'C':
                options->format = BX_LS_FORMAT_COLUMNS;
                break;
            case 'D':
                break;
            case 'G':
                break;
            case 'H':
                break;
            case 'I':
                break;
            case 'L':
                break;
            case 'N':
                break;
            case 'Q':
                break;
            case 'S':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_SIZE;
                break;
            case 'T':
                break;
            case 'X':
                break;
            case 'Z':
                break;
            case 'U':
                options->sort_entries = false;
                break;
            case 'a':
                options->show_all = true;
                options->almost_all = false;
                break;
            case 'b':
                options->escape_names = true;
                break;
            case 'c':
                break;
            case 'd':
                options->directory_mode = true;
                break;
            case 'F':
                options->classify = true;
                break;
            case 'f':
                options->sort_entries = false;
                options->show_all = true;
                options->almost_all = false;
                break;
            case 'g':
                options->format = BX_LS_FORMAT_LONG;
                break;
            case 'i':
                options->show_inode = true;
                break;
            case 'k':
                break;
            case 'l':
                options->format = BX_LS_FORMAT_LONG;
                break;
            case 'm':
                break;
            case 'n':
                options->format = BX_LS_FORMAT_LONG;
                options->numeric_ids = true;
                break;
            case 'o':
                options->format = BX_LS_FORMAT_LONG;
                break;
            case 'h':
                options->human_readable = true;
                break;
            case 'p':
                options->slash_directories = true;
                break;
            case 'q':
                break;
            case 'r':
                options->reverse_sort = true;
                break;
            case 'R':
                options->recursive = true;
                break;
            case 's':
                break;
            case 't':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_TIME;
                break;
            case 'u':
                break;
            case 'v':
                break;
            case 'w':
                break;
            case 'x':
                options->format = BX_LS_FORMAT_COLUMNS;
                break;
            case BX_LS_OPT_HELP:
                options->show_help = true;
                return true;
            case BX_LS_OPT_VERSION:
                options->show_version = true;
                return true;
            case BX_LS_OPT_FORMAT:
                if (!bx_ls_parse_format_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_COLOR:
                if (!bx_ls_parse_color_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_SI:
                options->human_readable = true;
                options->si_units = true;
                break;
            case BX_LS_OPT_AUTHOR:
                break;
            case BX_LS_OPT_BLOCK_SIZE:
                break;
            case BX_LS_OPT_FULL_TIME:
                options->format = BX_LS_FORMAT_LONG;
                break;
            case BX_LS_OPT_GROUP_DIRECTORIES_FIRST:
                break;
            case BX_LS_OPT_DEREFERENCE_CMDLINE_SYMLINK_TO_DIR:
                break;
            case BX_LS_OPT_HIDE:
                break;
            case BX_LS_OPT_HYPERLINK:
                break;
            case BX_LS_OPT_INDICATOR_STYLE:
                break;
            case BX_LS_OPT_SHOW_CONTROL_CHARS:
                break;
            case BX_LS_OPT_QUOTING_STYLE:
                break;
            case BX_LS_OPT_SORT:
                if (!bx_ls_parse_sort_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_TIME:
                break;
            case BX_LS_OPT_TIME_STYLE:
                break;
            case BX_LS_OPT_ZERO:
                break;
            case '?':
                if (optopt != 0) {
                    bx_diag(diag, "invalid option -- '%c'", optopt);
                }
                else if (optind > 0 && optind <= argc && argv[optind - 1] != NULL) {
                    bx_diag(diag, "unrecognized option '%s'", argv[optind - 1]);
                }
                else {
                    bx_diag(diag, "unrecognized option");
                }
                return false;
            default:
                return false;
        }
    }

    *first_operand = optind;
    return true;
}

static void bx_ls_entry_list_append(struct bx_ls_entry_list* list, struct bx_ls_entry* entry) {
    if (list->len == list->cap) {
        size_t new_cap = (list->cap == 0) ? 16u : list->cap * 2u;
        list->items = xrealloc(list->items, new_cap * sizeof(*list->items));
        list->cap = new_cap;
    }

    list->items[list->len++] = *entry;
    memset(entry, 0, sizeof(*entry));
}

static void bx_ls_entry_list_free(struct bx_ls_entry_list* list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].name);
        free(list->items[i].full_path);
    }

    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void bx_ls_path_list_append(struct bx_ls_path_list* list, const char* path) {
    if (list->len == list->cap) {
        size_t new_cap = (list->cap == 0) ? 8u : list->cap * 2u;
        list->items = xrealloc(list->items, new_cap * sizeof(*list->items));
        list->cap = new_cap;
    }

    list->items[list->len++] = xstrdup(path);
}

static void bx_ls_path_list_free(struct bx_ls_path_list* list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i]);
    }

    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static void bx_ls_perror_path(struct bx_diag_ctx* diag, const char* path, int status) {
    int err = errno;
    fprintf(stderr, "%s: %s: %s\n", diag->progname, path, strerror(err));
    if (diag->exit_status < status) {
        diag->exit_status = status;
    }
}

static const struct bx_ls_options* bx_ls_sort_options = NULL;

static int bx_ls_compare_intmax(intmax_t lhs, intmax_t rhs) {
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

static int bx_ls_entry_compare(const void* lhs, const void* rhs) {
    const struct bx_ls_entry* a = (const struct bx_ls_entry*)lhs;
    const struct bx_ls_entry* b = (const struct bx_ls_entry*)rhs;

    enum bx_ls_sort_mode sort_mode = BX_LS_SORT_NAME;
    bool reverse = false;
    if (bx_ls_sort_options != NULL) {
        sort_mode = bx_ls_sort_options->sort_mode;
        reverse = bx_ls_sort_options->reverse_sort;
    }

    int cmp = 0;
    if (sort_mode == BX_LS_SORT_TIME) {
        if (a->has_stat && b->has_stat) {
            cmp = bx_ls_compare_intmax((intmax_t)b->st.st_mtime, (intmax_t)a->st.st_mtime);
#if defined(__linux__)
            if (cmp == 0) {
                cmp = bx_ls_compare_intmax((intmax_t)b->st.st_mtim.tv_nsec, (intmax_t)a->st.st_mtim.tv_nsec);
            }
#endif
        }
        else if (a->has_stat != b->has_stat) {
            cmp = a->has_stat ? -1 : 1;
        }
    }
    else if (sort_mode == BX_LS_SORT_SIZE) {
        if (a->has_stat && b->has_stat) {
            cmp = bx_ls_compare_intmax((intmax_t)b->st.st_size, (intmax_t)a->st.st_size);
        }
        else if (a->has_stat != b->has_stat) {
            cmp = a->has_stat ? -1 : 1;
        }
    }

    if (cmp == 0) {
        cmp = strcmp(a->name, b->name);
    }

    return reverse ? -cmp : cmp;
}

static int bx_ls_path_compare(const void* lhs, const void* rhs) {
    const char* const* a = (const char* const*)lhs;
    const char* const* b = (const char* const*)rhs;

    int cmp = strcmp(*a, *b);
    if (bx_ls_sort_options != NULL && bx_ls_sort_options->reverse_sort) {
        cmp = -cmp;
    }
    return cmp;
}

static bool bx_ls_should_include_name(const char* name, const struct bx_ls_options* options) {
    if (options->show_all) {
        return true;
    }

    if (options->almost_all) {
        return strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
    }

    return name[0] != '.';
}

static char* bx_ls_join_path(const char* dir_path, const char* name) {
    size_t dir_len = strlen(dir_path);
    size_t name_len = strlen(name);
    bool needs_slash = dir_len > 0 && dir_path[dir_len - 1u] != '/';

    size_t total_len = dir_len + (needs_slash ? 1u : 0u) + name_len;
    char* joined = xmalloc(total_len + 1u);

    memcpy(joined, dir_path, dir_len);
    size_t out_pos = dir_len;
    if (needs_slash) {
        joined[out_pos++] = '/';
    }

    memcpy(joined + out_pos, name, name_len);
    joined[total_len] = '\0';
    return joined;
}

static bool bx_ls_collect_directory_entries(const char* dir_path, const struct bx_ls_options* options, struct bx_ls_entry_list* entries, struct bx_diag_ctx* diag, int error_status) {
    DIR* dir = opendir(dir_path);
    if (dir == NULL) {
        bx_ls_perror_path(diag, dir_path, error_status);
        return false;
    }

    bool ok = true;

    while (true) {
        errno = 0;
        struct dirent* dirent = readdir(dir);
        if (dirent == NULL) {
            if (errno != 0) {
                bx_ls_perror_path(diag, dir_path, error_status);
                ok = false;
            }
            break;
        }

        const char* name = dirent->d_name;
        if (!bx_ls_should_include_name(name, options)) {
            continue;
        }

        struct bx_ls_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.name = xstrdup(name);
        entry.full_path = bx_ls_join_path(dir_path, name);
        entry.has_stat = (lstat(entry.full_path, &entry.st) == 0);

        bx_ls_entry_list_append(entries, &entry);
    }

    if (closedir(dir) != 0) {
        bx_ls_perror_path(diag, dir_path, error_status);
        ok = false;
    }

    if (options->sort_entries && entries->len > 1) {
        bx_ls_sort_options = options;
        qsort(entries->items, entries->len, sizeof(entries->items[0]), bx_ls_entry_compare);
        bx_ls_sort_options = NULL;
    }

    return ok;
}

static char bx_ls_mode_type_char(mode_t mode) {
    if (S_ISREG(mode)) {
        return '-';
    }
    if (S_ISDIR(mode)) {
        return 'd';
    }
    if (S_ISLNK(mode)) {
        return 'l';
    }
    if (S_ISCHR(mode)) {
        return 'c';
    }
    if (S_ISBLK(mode)) {
        return 'b';
    }
    if (S_ISFIFO(mode)) {
        return 'p';
    }
#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        return 's';
    }
#endif
    return '?';
}

static void bx_ls_mode_to_string(mode_t mode, char out[11]) {
    out[0] = bx_ls_mode_type_char(mode);
    out[1] = (mode & S_IRUSR) ? 'r' : '-';
    out[2] = (mode & S_IWUSR) ? 'w' : '-';
    out[3] = (mode & S_IXUSR) ? 'x' : '-';
    out[4] = (mode & S_IRGRP) ? 'r' : '-';
    out[5] = (mode & S_IWGRP) ? 'w' : '-';
    out[6] = (mode & S_IXGRP) ? 'x' : '-';
    out[7] = (mode & S_IROTH) ? 'r' : '-';
    out[8] = (mode & S_IWOTH) ? 'w' : '-';
    out[9] = (mode & S_IXOTH) ? 'x' : '-';

    if (mode & S_ISUID) {
        out[3] = (mode & S_IXUSR) ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        out[6] = (mode & S_IXGRP) ? 's' : 'S';
    }
#ifdef S_ISVTX
    if (mode & S_ISVTX) {
        out[9] = (mode & S_IXOTH) ? 't' : 'T';
    }
#endif

    out[10] = '\0';
}

static char* bx_ls_readlink_target(const char* path) {
    size_t capacity = 128u;
    char* target = xmalloc(capacity + 1u);

    while (true) {
        ssize_t nread = readlink(path, target, capacity);
        if (nread < 0) {
            free(target);
            return NULL;
        }

        if ((size_t)nread < capacity) {
            target[nread] = '\0';
            return target;
        }

        if (capacity > (SIZE_MAX / 2u) - 1u) {
            free(target);
            errno = ENOMEM;
            return NULL;
        }

        capacity *= 2u;
        target = xrealloc(target, capacity + 1u);
    }
}

static const char* bx_ls_user_name(uid_t uid, bool numeric_ids, char numeric_buffer[32]) {
    if (!numeric_ids) {
        struct passwd* pw = getpwuid(uid);
        if (pw != NULL && pw->pw_name != NULL && pw->pw_name[0] != '\0') {
            return pw->pw_name;
        }
    }

    (void)snprintf(numeric_buffer, 32u, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

static const char* bx_ls_group_name(gid_t gid, bool numeric_ids, char numeric_buffer[32]) {
    if (!numeric_ids) {
        struct group* gr = getgrgid(gid);
        if (gr != NULL && gr->gr_name != NULL && gr->gr_name[0] != '\0') {
            return gr->gr_name;
        }
    }

    (void)snprintf(numeric_buffer, 32u, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}

static void bx_ls_format_timestamp(time_t timestamp, char buffer[32]) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        now = timestamp;
    }

    struct tm tm_value;
    if (localtime_r(&timestamp, &tm_value) == NULL) {
        (void)snprintf(buffer, 32u, "??? ?? ??:??");
        return;
    }

    double delta = difftime(now, timestamp);
    if (delta < 0.0) {
        delta = -delta;
    }

    const char* fmt = (delta > (365.0 / 2.0) * 24.0 * 60.0 * 60.0 || timestamp > now + 3600) ? "%b %e  %Y" : "%b %e %H:%M";
    if (strftime(buffer, 32u, fmt, &tm_value) == 0u) {
        (void)snprintf(buffer, 32u, "??? ?? ??:??");
    }
}

static size_t bx_ls_escape_append_octal(char* out, size_t out_pos, unsigned char ch) {
    out[out_pos++] = '\\';
    out[out_pos++] = (char)('0' + ((ch >> 6) & 7u));
    out[out_pos++] = (char)('0' + ((ch >> 3) & 7u));
    out[out_pos++] = (char)('0' + (ch & 7u));
    return out_pos;
}

static char* bx_ls_escape_name(const char* name, bool escape_names) {
    if (!escape_names) {
        return xstrdup(name);
    }

    size_t len = strlen(name);
    char* out = xmalloc((len * 4u) + 1u);
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        switch (ch) {
            case '\\':
                out[out_pos++] = '\\';
                out[out_pos++] = '\\';
                break;
            case '\a':
                out[out_pos++] = '\\';
                out[out_pos++] = 'a';
                break;
            case '\b':
                out[out_pos++] = '\\';
                out[out_pos++] = 'b';
                break;
            case '\f':
                out[out_pos++] = '\\';
                out[out_pos++] = 'f';
                break;
            case '\n':
                out[out_pos++] = '\\';
                out[out_pos++] = 'n';
                break;
            case '\r':
                out[out_pos++] = '\\';
                out[out_pos++] = 'r';
                break;
            case '\t':
                out[out_pos++] = '\\';
                out[out_pos++] = 't';
                break;
            case '\v':
                out[out_pos++] = '\\';
                out[out_pos++] = 'v';
                break;
            default:
                if (isprint(ch)) {
                    out[out_pos++] = (char)ch;
                }
                else {
                    out_pos = bx_ls_escape_append_octal(out, out_pos, ch);
                }
                break;
        }
    }

    out[out_pos] = '\0';
    return out;
}

static char bx_ls_indicator_char(mode_t mode, const struct bx_ls_options* options) {
    if (options->classify) {
        if (S_ISDIR(mode)) {
            return '/';
        }
        if (S_ISLNK(mode)) {
            return '@';
        }
        if (S_ISFIFO(mode)) {
            return '|';
        }
#ifdef S_ISSOCK
        if (S_ISSOCK(mode)) {
            return '=';
        }
#endif
        if (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
            return '*';
        }
    }

    if (options->slash_directories && S_ISDIR(mode)) {
        return '/';
    }

    return '\0';
}

static bool bx_ls_color_enabled(const struct bx_ls_options* options) {
    switch (options->color_when) {
        case BX_LS_COLOR_ALWAYS:
            return true;
        case BX_LS_COLOR_AUTO:
            return isatty(STDOUT_FILENO);
        case BX_LS_COLOR_NEVER:
        default:
            return false;
    }
}

static bool bx_ls_lookup_ls_colors_key(const char* key, char* buffer, size_t buffer_size) {
    if (buffer_size == 0u) {
        return false;
    }

    const char* ls_colors = getenv("LS_COLORS");
    if (ls_colors == NULL || ls_colors[0] == '\0') {
        return false;
    }

    size_t key_len = strlen(key);
    const char* cursor = ls_colors;

    const char* match_value = NULL;
    size_t match_len = 0u;

    while (*cursor != '\0') {
        const char* entry_start = cursor;
        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        const char* entry_end = cursor;

        const char* equal = memchr(entry_start, '=', (size_t)(entry_end - entry_start));
        if (equal != NULL && (size_t)(equal - entry_start) == key_len && strncmp(entry_start, key, key_len) == 0) {
            match_value = equal + 1;
            match_len = (size_t)(entry_end - equal - 1);
        }

        if (*cursor == ':') {
            cursor++;
        }
    }

    if (match_value == NULL) {
        return false;
    }

    if (match_len >= buffer_size) {
        match_len = buffer_size - 1u;
    }
    memcpy(buffer, match_value, match_len);
    buffer[match_len] = '\0';
    return true;
}

static bool bx_ls_name_has_suffix(const char* name, size_t name_len, const char* suffix, size_t suffix_len) {
    if (suffix_len > name_len) {
        return false;
    }

    return memcmp(name + (name_len - suffix_len), suffix, suffix_len) == 0;
}

static bool bx_ls_lookup_ls_colors_suffix(const char* name, char* buffer, size_t buffer_size) {
    if (buffer_size == 0u) {
        return false;
    }

    const char* ls_colors = getenv("LS_COLORS");
    if (ls_colors == NULL || ls_colors[0] == '\0') {
        return false;
    }

    const size_t name_len = strlen(name);
    const char* cursor = ls_colors;
    const char* match_value = NULL;
    size_t match_len = 0u;

    while (*cursor != '\0') {
        const char* entry_start = cursor;
        while (*cursor != '\0' && *cursor != ':') {
            cursor++;
        }
        const char* entry_end = cursor;

        const char* equal = memchr(entry_start, '=', (size_t)(entry_end - entry_start));
        if (equal != NULL && entry_start < equal && entry_start[0] == '*') {
            const char* suffix = entry_start + 1;
            size_t suffix_len = (size_t)(equal - suffix);
            if (suffix_len != 0u && bx_ls_name_has_suffix(name, name_len, suffix, suffix_len)) {
                match_value = equal + 1;
                match_len = (size_t)(entry_end - equal - 1);
            }
        }

        if (*cursor == ':') {
            cursor++;
        }
    }

    if (match_value == NULL) {
        return false;
    }

    if (match_len >= buffer_size) {
        match_len = buffer_size - 1u;
    }
    memcpy(buffer, match_value, match_len);
    buffer[match_len] = '\0';
    return true;
}

static const char* bx_ls_default_color_code(const char* key) {
    (void)key;
    return "";
}

static const char* bx_ls_color_code_for_key(const char* key, char* buffer, size_t buffer_size) {
    if (bx_ls_lookup_ls_colors_key(key, buffer, buffer_size)) {
        return buffer;
    }
    return bx_ls_default_color_code(key);
}

static const char* bx_ls_color_key_for_mode(mode_t mode) {
    if (S_ISDIR(mode)) {
        bool sticky = false;
#ifdef S_ISVTX
        sticky = (mode & S_ISVTX) != 0;
#endif
        bool world_writable = (mode & S_IWOTH) != 0;
        if (world_writable && sticky) {
            return "tw";
        }
        if (world_writable) {
            return "ow";
        }
        if (sticky) {
            return "st";
        }
        return "di";
    }

    if (S_ISFIFO(mode)) {
        return "pi";
    }
#ifdef S_ISSOCK
    if (S_ISSOCK(mode)) {
        return "so";
    }
#endif
#ifdef S_ISDOOR
    if (S_ISDOOR(mode)) {
        return "do";
    }
#endif
    if (S_ISBLK(mode)) {
        return "bd";
    }
    if (S_ISCHR(mode)) {
        return "cd";
    }

    return "no";
}

static const char* bx_ls_color_code_for_regular(const struct bx_ls_entry* entry, const struct stat* st, char* buffer, size_t buffer_size) {
    mode_t mode = st->st_mode;

    if ((mode & S_ISUID) != 0) {
        return bx_ls_color_code_for_key("su", buffer, buffer_size);
    }
    if ((mode & S_ISGID) != 0) {
        return bx_ls_color_code_for_key("sg", buffer, buffer_size);
    }
    if ((mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
        return bx_ls_color_code_for_key("ex", buffer, buffer_size);
    }
    if (st->st_nlink > 1u) {
        return bx_ls_color_code_for_key("mh", buffer, buffer_size);
    }
    if (bx_ls_lookup_ls_colors_suffix(entry->name, buffer, buffer_size)) {
        return buffer;
    }

    return bx_ls_color_code_for_key("fi", buffer, buffer_size);
}

static const char* bx_ls_color_code_for_entry(const struct bx_ls_entry* entry, const struct stat* st, char* buffer, size_t buffer_size) {
    mode_t mode = st->st_mode;

    if (S_ISLNK(mode)) {
        struct stat target_stat;
        if (stat(entry->full_path, &target_stat) != 0) {
            return bx_ls_color_code_for_key("or", buffer, buffer_size);
        }

        char link_buffer[64];
        if (bx_ls_lookup_ls_colors_key("ln", link_buffer, sizeof(link_buffer)) && strcmp(link_buffer, "target") == 0) {
            if (S_ISREG(target_stat.st_mode)) {
                return bx_ls_color_code_for_regular(entry, &target_stat, buffer, buffer_size);
            }
            return bx_ls_color_code_for_key(bx_ls_color_key_for_mode(target_stat.st_mode), buffer, buffer_size);
        }

        return bx_ls_color_code_for_key("ln", buffer, buffer_size);
    }

    if (S_ISREG(mode)) {
        return bx_ls_color_code_for_regular(entry, st, buffer, buffer_size);
    }

    return bx_ls_color_code_for_key(bx_ls_color_key_for_mode(mode), buffer, buffer_size);
}

static char* bx_ls_colorize_name(const char* text, const struct bx_ls_entry* entry, const struct stat* st, const struct bx_ls_options* options) {
    if (!bx_ls_color_enabled(options)) {
        return xstrdup(text);
    }

    char color_buffer[128];
    const char* color = bx_ls_color_code_for_entry(entry, st, color_buffer, sizeof(color_buffer));
    if (color == NULL || color[0] == '\0') {
        return xstrdup(text);
    }

    char reset_buffer[64];
    const char* reset = bx_ls_color_code_for_key("rs", reset_buffer, sizeof(reset_buffer));
    if (reset == NULL || reset[0] == '\0') {
        reset = "0";
    }

    size_t color_len = strlen(color);
    size_t text_len = strlen(text);
    size_t reset_len = strlen(reset);
    size_t output_len = 2u + color_len + 1u + text_len + 2u + reset_len + 1u;
    char* output = xmalloc(output_len + 1u);
    size_t out_pos = 0;

    output[out_pos++] = '\033';
    output[out_pos++] = '[';
    memcpy(output + out_pos, color, color_len);
    out_pos += color_len;
    output[out_pos++] = 'm';

    memcpy(output + out_pos, text, text_len);
    out_pos += text_len;

    output[out_pos++] = '\033';
    output[out_pos++] = '[';
    memcpy(output + out_pos, reset, reset_len);
    out_pos += reset_len;
    output[out_pos++] = 'm';
    output[out_pos] = '\0';
    return output;
}

static void bx_ls_format_size(intmax_t size, const struct bx_ls_options* options, char buffer[32]) {
    if (!options->human_readable) {
        (void)snprintf(buffer, 32u, "%" PRIdMAX, size);
        return;
    }

    static const char* units_1024[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    static const char* units_1000[] = {"", "k", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    const char* const* units = options->si_units ? units_1000 : units_1024;
    const double base = options->si_units ? 1000.0 : 1024.0;
    const size_t max_unit = (sizeof(units_1024) / sizeof(units_1024[0])) - 1u;

    bool negative = size < 0;
    uintmax_t magnitude = (uintmax_t)size;
    if (negative) {
        magnitude = (uintmax_t)(-(size + 1)) + 1u;
    }

    double value = (double)magnitude;
    size_t unit = 0;

    while (value >= base && unit < max_unit) {
        value /= base;
        unit++;
    }

    if (unit == 0u) {
        (void)snprintf(buffer, 32u, "%" PRIdMAX, size);
        return;
    }

    const char* sign = negative ? "-" : "";
    if (value < 10.0) {
        (void)snprintf(buffer, 32u, "%s%.1f%s", sign, value, units[unit]);
    }
    else {
        (void)snprintf(buffer, 32u, "%s%.0f%s", sign, value, units[unit]);
    }
}

static char* bx_ls_append_indicator(char* name, char indicator) {
    if (indicator == '\0') {
        return name;
    }

    size_t len = strlen(name);
    char* out = xmalloc(len + 2u);
    memcpy(out, name, len);
    out[len] = indicator;
    out[len + 1u] = '\0';
    free(name);
    return out;
}

static bool bx_ls_entry_stat(const struct bx_ls_entry* entry, struct stat* st, struct bx_diag_ctx* diag, bool emit_error) {
    if (entry->has_stat) {
        *st = entry->st;
        return true;
    }

    if (lstat(entry->full_path, st) == 0) {
        return true;
    }

    if (emit_error) {
        bx_ls_perror_path(diag, entry->full_path, 1);
    }
    return false;
}

static size_t bx_ls_uintmax_width(uintmax_t value) {
    size_t width = 1u;
    while (value >= 10u) {
        value /= 10u;
        width++;
    }
    return width;
}

static void bx_ls_long_widths_init(struct bx_ls_long_widths* widths, const struct bx_ls_options* options) {
    widths->inode = options->show_inode ? 1u : 0u;
    widths->nlink = 1u;
    widths->user = 1u;
    widths->group = 1u;
    widths->size = 1u;
}

static void bx_ls_compute_long_widths(
    const struct bx_ls_entry_list* entries,
    const struct bx_ls_options* options,
    struct bx_diag_ctx* diag,
    struct bx_ls_long_widths* widths) {
    bx_ls_long_widths_init(widths, options);

    for (size_t i = 0; i < entries->len; i++) {
        const struct bx_ls_entry* entry = &entries->items[i];
        struct stat st;
        if (!bx_ls_entry_stat(entry, &st, diag, false)) {
            continue;
        }

        if (options->show_inode) {
            size_t inode_width = bx_ls_uintmax_width((uintmax_t)st.st_ino);
            if (inode_width > widths->inode) {
                widths->inode = inode_width;
            }
        }

        size_t nlink_width = bx_ls_uintmax_width((uintmax_t)st.st_nlink);
        if (nlink_width > widths->nlink) {
            widths->nlink = nlink_width;
        }

        char user_numeric[32];
        const char* user_name = bx_ls_user_name(st.st_uid, options->numeric_ids, user_numeric);
        size_t user_width = strlen(user_name);
        if (user_width > widths->user) {
            widths->user = user_width;
        }

        char group_numeric[32];
        const char* group_name = bx_ls_group_name(st.st_gid, options->numeric_ids, group_numeric);
        size_t group_width = strlen(group_name);
        if (group_width > widths->group) {
            widths->group = group_width;
        }

        char size_text[32];
        bx_ls_format_size((intmax_t)st.st_size, options, size_text);
        size_t size_width = strlen(size_text);
        if (size_width > widths->size) {
            widths->size = size_width;
        }
    }
}

static bool bx_ls_build_short_cell(const struct bx_ls_entry* entry, const struct bx_ls_options* options, struct bx_diag_ctx* diag, char** out_cell) {
    struct stat st;
    bool have_stat = bx_ls_entry_stat(entry, &st, diag, options->show_inode);

    char* name = bx_ls_escape_name(entry->name, options->escape_names);
    if (have_stat) {
        name = bx_ls_append_indicator(name, bx_ls_indicator_char(st.st_mode, options));
        char* colored_name = bx_ls_colorize_name(name, entry, &st, options);
        free(name);
        name = colored_name;
    }

    if (!options->show_inode) {
        *out_cell = name;
        return true;
    }

    if (!have_stat) {
        free(name);
        return false;
    }

    char inode_prefix[64];
    (void)snprintf(inode_prefix, sizeof(inode_prefix), "%" PRIuMAX " ", (uintmax_t)st.st_ino);
    size_t prefix_len = strlen(inode_prefix);
    size_t name_len = strlen(name);
    char* combined = xmalloc(prefix_len + name_len + 1u);
    memcpy(combined, inode_prefix, prefix_len);
    memcpy(combined + prefix_len, name, name_len + 1u);
    free(name);
    *out_cell = combined;
    return true;
}

static size_t bx_ls_output_width(void) {
    const char* columns_env = getenv("COLUMNS");
    if (columns_env != NULL && columns_env[0] != '\0') {
        char* end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(columns_env, &end, 10);
        if (errno == 0 && end != columns_env && *end == '\0' && parsed > 0ul) {
            return (size_t)parsed;
        }
    }

    bool stdout_is_tty = (isatty(STDOUT_FILENO) == 1);
    struct winsize ws;
    if (stdout_is_tty && ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (size_t)ws.ws_col;
    }

    if (!stdout_is_tty) {
        return SIZE_MAX;
    }

    return 80u;
}

static size_t bx_ls_display_width(const char* text) {
    size_t width = 0;

    for (size_t i = 0; text[i] != '\0';) {
        if ((unsigned char)text[i] == 0x1b && text[i + 1u] == '[') {
            i += 2u;
            while (text[i] != '\0') {
                unsigned char ch = (unsigned char)text[i];
                i++;
                if (ch >= 0x40u && ch <= 0x7eu) {
                    break;
                }
            }
            continue;
        }

        i++;
        width++;
    }

    return width;
}

static void bx_ls_print_entries_single(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    for (size_t i = 0; i < entries->len; i++) {
        char* cell = NULL;
        if (!bx_ls_build_short_cell(&entries->items[i], options, diag, &cell)) {
            continue;
        }

        (void)fputs(cell, stdout);
        (void)fputc('\n', stdout);
        free(cell);
    }
}

static void bx_ls_print_entries_columns(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (entries->len == 0) {
        return;
    }

    char** cells = xmalloc(entries->len * sizeof(*cells));
    size_t cell_count = 0;
    size_t max_width = 0;

    for (size_t i = 0; i < entries->len; i++) {
        char* cell = NULL;
        if (!bx_ls_build_short_cell(&entries->items[i], options, diag, &cell)) {
            continue;
        }

        cells[cell_count++] = cell;
        size_t cell_width = bx_ls_display_width(cell);
        if (cell_width > max_width) {
            max_width = cell_width;
        }
    }

    if (cell_count == 0) {
        free(cells);
        return;
    }

    size_t term_width = bx_ls_output_width();
    size_t column_width = max_width + 2u;
    size_t column_count = (column_width == 0u) ? 1u : (term_width / column_width);
    if (column_count == 0u) {
        column_count = 1u;
    }
    if (column_count > cell_count) {
        column_count = cell_count;
    }
    size_t row_count = (cell_count + column_count - 1u) / column_count;
    size_t* column_widths = xmalloc(column_count * sizeof(*column_widths));
    for (size_t col = 0; col < column_count; col++) {
        column_widths[col] = 0u;
        for (size_t row = 0; row < row_count; row++) {
            size_t idx = col * row_count + row;
            if (idx >= cell_count) {
                continue;
            }
            size_t used = bx_ls_display_width(cells[idx]);
            if (used > column_widths[col]) {
                column_widths[col] = used;
            }
        }
    }

    for (size_t row = 0; row < row_count; row++) {
        for (size_t col = 0; col < column_count; col++) {
            size_t idx = col * row_count + row;
            if (idx >= cell_count) {
                continue;
            }

            (void)fputs(cells[idx], stdout);

            size_t next_col = col + 1u;
            while (next_col < column_count) {
                size_t next_idx = next_col * row_count + row;
                if (next_idx < cell_count) {
                    break;
                }
                next_col++;
            }

            if (next_col < column_count) {
                size_t used = bx_ls_display_width(cells[idx]);
                size_t pad = (column_widths[col] > used) ? (column_widths[col] - used) : 0u;
                pad += 2u;
                for (size_t p = 0; p < pad; p++) {
                    (void)fputc(' ', stdout);
                }
            }
        }
        (void)fputc('\n', stdout);
    }

    for (size_t i = 0; i < cell_count; i++) {
        free(cells[i]);
    }
    free(column_widths);
    free(cells);
}

static void bx_ls_print_long_entry(
    const struct bx_ls_entry* entry,
    const struct bx_ls_options* options,
    const struct bx_ls_long_widths* widths,
    struct bx_diag_ctx* diag) {
    struct stat st;
    if (!bx_ls_entry_stat(entry, &st, diag, true)) {
        return;
    }

    char mode[11];
    bx_ls_mode_to_string(st.st_mode, mode);

    char user_numeric[32];
    char group_numeric[32];
    const char* user_name = bx_ls_user_name(st.st_uid, options->numeric_ids, user_numeric);
    const char* group_name = bx_ls_group_name(st.st_gid, options->numeric_ids, group_numeric);

    char timestamp[32];
    bx_ls_format_timestamp(st.st_mtime, timestamp);
    char size[32];
    bx_ls_format_size((intmax_t)st.st_size, options, size);

    char* display_name = bx_ls_escape_name(entry->name, options->escape_names);
    display_name = bx_ls_append_indicator(display_name, bx_ls_indicator_char(st.st_mode, options));
    char* colored_name = bx_ls_colorize_name(display_name, entry, &st, options);
    free(display_name);
    display_name = colored_name;

    char* symlink_display = NULL;
    if (S_ISLNK(st.st_mode)) {
        char* symlink_target = bx_ls_readlink_target(entry->full_path);
        if (symlink_target == NULL) {
            bx_ls_perror_path(diag, entry->full_path, 1);
        }
        else {
            symlink_display = bx_ls_escape_name(symlink_target, options->escape_names);
            free(symlink_target);
        }
    }

    if (options->show_inode) {
        printf("%*" PRIuMAX " ", (int)widths->inode, (uintmax_t)st.st_ino);
    }

    printf(
        "%s %*" PRIuMAX " %-*s %-*s %*s %s %s",
        mode,
        (int)widths->nlink,
        (uintmax_t)st.st_nlink,
        (int)widths->user,
        user_name,
        (int)widths->group,
        group_name,
        (int)widths->size,
        size,
        timestamp,
        display_name);

    if (symlink_display != NULL) {
        printf(" -> %s", symlink_display);
    }
    (void)fputc('\n', stdout);

    free(display_name);
    free(symlink_display);
}

static uintmax_t bx_ls_total_blocks_kib(const struct bx_ls_entry_list* entries, struct bx_diag_ctx* diag) {
    uintmax_t total_blocks = 0;

    for (size_t i = 0; i < entries->len; i++) {
        struct stat st;
        if (!bx_ls_entry_stat(&entries->items[i], &st, diag, false)) {
            continue;
        }

        if (st.st_blocks > 0) {
            total_blocks += (uintmax_t)st.st_blocks;
        }
    }

    return (total_blocks + 1u) / 2u;
}

static void bx_ls_print_entries_long(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_total) {
    struct bx_ls_long_widths widths;
    bx_ls_compute_long_widths(entries, options, diag, &widths);

    if (print_total) {
        printf("total %" PRIuMAX "\n", bx_ls_total_blocks_kib(entries, diag));
    }

    for (size_t i = 0; i < entries->len; i++) {
        bx_ls_print_long_entry(&entries->items[i], options, &widths, diag);
    }
}

static void bx_ls_print_entries(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_total) {
    switch (options->format) {
        case BX_LS_FORMAT_LONG:
            bx_ls_print_entries_long(entries, options, diag, print_total);
            break;
        case BX_LS_FORMAT_COLUMNS:
            bx_ls_print_entries_columns(entries, options, diag);
            break;
        case BX_LS_FORMAT_SINGLE:
        default:
            bx_ls_print_entries_single(entries, options, diag);
            break;
    }
}

static void bx_ls_list_directory(const char* dir_path, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_header, bool is_command_line_dir) {
    if (print_header) {
        printf("%s:\n", dir_path);
    }

    struct bx_ls_entry_list entries = {0};
    int error_status = is_command_line_dir ? 2 : 1;
    (void)bx_ls_collect_directory_entries(dir_path, options, &entries, diag, error_status);
    bx_ls_print_entries(&entries, options, diag, options->format == BX_LS_FORMAT_LONG);

    if (options->recursive) {
        for (size_t i = 0; i < entries.len; i++) {
            struct bx_ls_entry* entry = &entries.items[i];
            if (!entry->has_stat || !S_ISDIR(entry->st.st_mode)) {
                continue;
            }
            if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
                continue;
            }

            (void)fputc('\n', stdout);
            bx_ls_list_directory(entry->full_path, options, diag, true, false);
        }
    }

    bx_ls_entry_list_free(&entries);
}

static void bx_ls_add_operand_as_entry(const char* operand, const struct stat* st, struct bx_ls_entry_list* file_entries) {
    struct bx_ls_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = xstrdup(operand);
    entry.full_path = xstrdup(operand);
    entry.st = *st;
    entry.has_stat = true;
    bx_ls_entry_list_append(file_entries, &entry);
}

static void bx_ls_classify_operand(const char* operand, const struct bx_ls_options* options, struct bx_ls_entry_list* file_entries, struct bx_ls_path_list* directory_paths, struct bx_diag_ctx* diag) {
    struct stat st;
    if (lstat(operand, &st) != 0) {
        bx_ls_perror_path(diag, operand, 2);
        return;
    }

    if (S_ISDIR(st.st_mode) && !options->directory_mode) {
        bx_ls_path_list_append(directory_paths, operand);
        return;
    }

    bx_ls_add_operand_as_entry(operand, &st, file_entries);
}

static void bx_ls_run(const struct bx_ls_options* options, int argc, char** argv, int first_operand, struct bx_diag_ctx* diag) {
    struct bx_ls_entry_list file_entries = {0};
    struct bx_ls_path_list directory_paths = {0};

    if (first_operand >= argc) {
        bx_ls_classify_operand(".", options, &file_entries, &directory_paths, diag);
    }
    else {
        for (int i = first_operand; i < argc; i++) {
            bx_ls_classify_operand(argv[i], options, &file_entries, &directory_paths, diag);
        }
    }

    if (options->sort_entries) {
        if (file_entries.len > 1) {
            bx_ls_sort_options = options;
            qsort(file_entries.items, file_entries.len, sizeof(file_entries.items[0]), bx_ls_entry_compare);
            bx_ls_sort_options = NULL;
        }
        if (directory_paths.len > 1) {
            bx_ls_sort_options = options;
            qsort(directory_paths.items, directory_paths.len, sizeof(directory_paths.items[0]), bx_ls_path_compare);
            bx_ls_sort_options = NULL;
        }
    }

    bool wrote_output = false;

    if (file_entries.len > 0) {
        bx_ls_print_entries(&file_entries, options, diag, false);
        wrote_output = true;
    }

    bool print_directory_headers = options->recursive || file_entries.len > 0 || directory_paths.len > 1;
    for (size_t i = 0; i < directory_paths.len; i++) {
        if (wrote_output) {
            (void)fputc('\n', stdout);
        }
        bx_ls_list_directory(directory_paths.items[i], options, diag, print_directory_headers, true);
        wrote_output = true;
    }

    bx_ls_entry_list_free(&file_entries);
    bx_ls_path_list_free(&directory_paths);
}

static int bx_ls_main_variant(int argc, char** argv, enum bx_ls_variant variant) {
    struct bx_ls_options options;
    struct bx_diag_ctx diag = {
        .progname = bx_ls_variant_name(variant),
        .exit_status = 0,
        .verbose = false,
        .debug = false,
    };
    int first_operand = 0;

    if (!bx_ls_parse_options(argc, argv, variant, &options, &first_operand, &diag)) {
        return diag.exit_status != 0 ? diag.exit_status : 1;
    }

    if (options.show_help) {
        bx_ls_print_help(stdout, &options);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        return 0;
    }

    bx_ls_run(&options, argc, argv, first_operand, &diag);

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    return diag.exit_status;
}

int bx_ls_main(int argc, char** argv) {
    return bx_ls_main_variant(argc, argv, BX_LS_VARIANT_LS);
}

int bx_dir_main(int argc, char** argv) {
    return bx_ls_main_variant(argc, argv, BX_LS_VARIANT_DIR);
}

int bx_vdir_main(int argc, char** argv) {
    return bx_ls_main_variant(argc, argv, BX_LS_VARIANT_VDIR);
}
