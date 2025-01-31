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
#include "diag.h"
#include "libbx.h"

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
    bool show_help;
    bool show_version;
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

static const char* bx_ls_progname(const char* argv0, enum bx_ls_variant variant) {
    if (argv0 == NULL || argv0[0] == '\0') {
        return bx_ls_variant_name(variant);
    }

    const char* base = strrchr(argv0, '/');
    if (base != NULL && base[1] != '\0') {
        return base + 1;
    }

    return argv0;
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

static void bx_ls_print_version(const char* progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
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
    options->progname = bx_ls_progname(argv0, variant);
    options->variant = variant;
    options->format = bx_ls_default_format(variant);
    options->sort_entries = true;
    options->escape_names = (variant != BX_LS_VARIANT_LS);
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
                break;
            case 'R':
                options->recursive = true;
                break;
            case 's':
                break;
            case 't':
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

static int bx_ls_entry_compare_name(const void* lhs, const void* rhs) {
    const struct bx_ls_entry* a = (const struct bx_ls_entry*)lhs;
    const struct bx_ls_entry* b = (const struct bx_ls_entry*)rhs;
    return strcmp(a->name, b->name);
}

static int bx_ls_path_compare(const void* lhs, const void* rhs) {
    const char* const* a = (const char* const*)lhs;
    const char* const* b = (const char* const*)rhs;
    return strcmp(*a, *b);
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
    if (strcmp(dir_path, ".") == 0) {
        return xstrdup(name);
    }

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

static bool bx_ls_collect_directory_entries(const char* dir_path, const struct bx_ls_options* options, struct bx_ls_entry_list* entries, struct bx_diag_ctx* diag) {
    DIR* dir = opendir(dir_path);
    if (dir == NULL) {
        bx_perror_path(diag, dir_path);
        return false;
    }

    bool ok = true;

    while (true) {
        errno = 0;
        struct dirent* dirent = readdir(dir);
        if (dirent == NULL) {
            if (errno != 0) {
                bx_perror_path(diag, dir_path);
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
        bx_perror_path(diag, dir_path);
        ok = false;
    }

    if (options->sort_entries && entries->len > 1) {
        qsort(entries->items, entries->len, sizeof(entries->items[0]), bx_ls_entry_compare_name);
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
        bx_perror_path(diag, entry->full_path);
    }
    return false;
}

static bool bx_ls_build_short_cell(const struct bx_ls_entry* entry, const struct bx_ls_options* options, struct bx_diag_ctx* diag, char** out_cell) {
    struct stat st;
    bool have_stat = bx_ls_entry_stat(entry, &st, diag, options->show_inode);

    char* name = bx_ls_escape_name(entry->name, options->escape_names);
    if (have_stat) {
        name = bx_ls_append_indicator(name, bx_ls_indicator_char(st.st_mode, options));
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

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return (size_t)ws.ws_col;
    }

    return 80u;
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
        size_t cell_width = strlen(cell);
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
    size_t row_count = (cell_count + column_count - 1u) / column_count;

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
                size_t used = strlen(cells[idx]);
                size_t pad = (column_width > used) ? (column_width - used) : 1u;
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
    free(cells);
}

static void bx_ls_print_long_entry(const struct bx_ls_entry* entry, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
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

    char* symlink_display = NULL;
    if (S_ISLNK(st.st_mode)) {
        char* symlink_target = bx_ls_readlink_target(entry->full_path);
        if (symlink_target == NULL) {
            bx_perror_path(diag, entry->full_path);
        }
        else {
            symlink_display = bx_ls_escape_name(symlink_target, options->escape_names);
            free(symlink_target);
        }
    }

    if (options->show_inode) {
        printf("%9" PRIuMAX " ", (uintmax_t)st.st_ino);
    }

    printf("%s %3" PRIuMAX " %-8s %-8s %8s %s %s", mode, (uintmax_t)st.st_nlink, user_name, group_name, size, timestamp, display_name);

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
    if (print_total) {
        printf("total %" PRIuMAX "\n", bx_ls_total_blocks_kib(entries, diag));
    }

    for (size_t i = 0; i < entries->len; i++) {
        bx_ls_print_long_entry(&entries->items[i], options, diag);
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

static void bx_ls_list_directory(const char* dir_path, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_header) {
    if (print_header) {
        printf("%s:\n", dir_path);
    }

    struct bx_ls_entry_list entries = {0};
    (void)bx_ls_collect_directory_entries(dir_path, options, &entries, diag);
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
            bx_ls_list_directory(entry->full_path, options, diag, true);
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
        bx_perror_path(diag, operand);
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
            qsort(file_entries.items, file_entries.len, sizeof(file_entries.items[0]), bx_ls_entry_compare_name);
        }
        if (directory_paths.len > 1) {
            qsort(directory_paths.items, directory_paths.len, sizeof(directory_paths.items[0]), bx_ls_path_compare);
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
        bx_ls_list_directory(directory_paths.items[i], options, diag, print_directory_headers);
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
        bx_ls_print_version(options.progname);
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
