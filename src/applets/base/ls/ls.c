#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
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
#include "lib/size_parse.h"
#include "lib/args_common.h"

char* realpath(const char* restrict path, char* restrict resolved_path);

enum bx_ls_variant {
    BX_LS_VARIANT_LS,
    BX_LS_VARIANT_DIR,
    BX_LS_VARIANT_VDIR,
};

enum bx_ls_format {
    BX_LS_FORMAT_SINGLE,
    BX_LS_FORMAT_COLUMNS,
    BX_LS_FORMAT_LONG,
    BX_LS_FORMAT_COMMAS,
};

enum bx_ls_columns_layout {
    BX_LS_COLUMNS_VERTICAL = 0,
    BX_LS_COLUMNS_HORIZONTAL,
};

enum bx_ls_sort_mode {
    BX_LS_SORT_NAME = 0,
    BX_LS_SORT_TIME,
    BX_LS_SORT_SIZE,
    BX_LS_SORT_EXTENSION,
    BX_LS_SORT_VERSION,
    BX_LS_SORT_WIDTH,
};

enum bx_ls_indicator_style {
    BX_LS_INDICATOR_NONE = 0,
    BX_LS_INDICATOR_SLASH,
    BX_LS_INDICATOR_FILE_TYPE,
    BX_LS_INDICATOR_CLASSIFY,
};

enum bx_ls_time_kind {
    BX_LS_TIME_MTIME = 0,
    BX_LS_TIME_CTIME,
    BX_LS_TIME_ATIME,
    BX_LS_TIME_BIRTH,
};

enum bx_ls_time_style {
    BX_LS_TIME_STYLE_DEFAULT = 0,
    BX_LS_TIME_STYLE_FULL_ISO,
    BX_LS_TIME_STYLE_LONG_ISO,
    BX_LS_TIME_STYLE_ISO,
    BX_LS_TIME_STYLE_LOCALE,
    BX_LS_TIME_STYLE_CUSTOM,
};

enum bx_ls_quoting_style {
    BX_LS_QUOTING_DEFAULT = 0,
    BX_LS_QUOTING_LITERAL,
    BX_LS_QUOTING_LOCALE,
    BX_LS_QUOTING_SHELL,
    BX_LS_QUOTING_SHELL_ALWAYS,
    BX_LS_QUOTING_SHELL_ESCAPE,
    BX_LS_QUOTING_SHELL_ESCAPE_ALWAYS,
    BX_LS_QUOTING_C,
    BX_LS_QUOTING_ESCAPE,
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
    BX_LS_OPT_FILE_TYPE = 20,
};

enum bx_ls_color_when {
    BX_LS_COLOR_NEVER = 0,
    BX_LS_COLOR_ALWAYS,
    BX_LS_COLOR_AUTO,
};

enum bx_ls_hyperlink_when {
    BX_LS_HYPERLINK_NEVER = 0,
    BX_LS_HYPERLINK_ALWAYS,
    BX_LS_HYPERLINK_AUTO,
};

struct bx_ls_pattern_list {
    char** items;
    size_t len;
    size_t cap;
};

struct bx_ls_options {
    const char* progname;
    enum bx_ls_variant variant;
    enum bx_ls_format format;
    enum bx_ls_columns_layout columns_layout;
    bool show_all;
    bool almost_all;
    bool ignore_backups;
    bool directory_mode;
    bool recursive;
    bool dereference_all;
    bool dereference_command_line;
    bool dereference_command_line_symlink_to_dir;
    bool dired;
    bool show_owner;
    bool show_group;
    bool show_author;
    bool show_inode;
    bool show_size_blocks;
    bool show_context;
    bool numeric_ids;
    bool human_readable;
    bool si_units;
    bool escape_names;
    bool hide_control_chars;
    bool sort_entries;
    bool reverse_sort;
    enum bx_ls_sort_mode sort_mode;
    bool show_help;
    bool show_version;
    bool zero_terminated;
    bool width_set;
    size_t output_width;
    size_t tabsize;
    bool block_size_set;
    uintmax_t block_size_divisor;
    char block_size_suffix[8];
    enum bx_ls_color_when color_when;
    enum bx_ls_hyperlink_when hyperlink_when;
    enum bx_ls_indicator_style indicator_style;
    enum bx_ls_time_kind time_kind;
    enum bx_ls_time_style time_style;
    enum bx_ls_quoting_style quoting_style;
    char* custom_time_style;
    struct bx_ls_pattern_list ignore_patterns;
    struct bx_ls_pattern_list hide_patterns;
};

struct bx_ls_entry {
    char* name;
    char* full_path;
    struct stat st;
    bool has_stat;
    bool follow_for_display;
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
    size_t blocks;
    size_t nlink;
    size_t user;
    size_t group;
    size_t author;
    size_t context;
    size_t size;
};

struct bx_ls_short_widths {
    size_t inode;
    size_t blocks;
};

struct bx_ls_dir_identity {
    dev_t dev;
    ino_t ino;
};

struct bx_ls_dir_stack {
    struct bx_ls_dir_identity* items;
    size_t len;
    size_t cap;
};

struct bx_ls_dired_range {
    size_t start;
    size_t end;
};

struct bx_ls_dired_output {
    FILE* stream;
    char* data;
    size_t len;
    struct bx_ls_dired_range* name_ranges;
    size_t name_len;
    size_t name_cap;
    struct bx_ls_dired_range* subdir_ranges;
    size_t subdir_len;
    size_t subdir_cap;
};

static void bx_ls_pattern_list_append(struct bx_ls_pattern_list* list, char* pattern);
static void bx_ls_pattern_list_free(struct bx_ls_pattern_list* list);
static time_t bx_ls_selected_time_sec(const struct stat* st, enum bx_ls_time_kind kind);
static long bx_ls_selected_time_nsec(const struct stat* st, enum bx_ls_time_kind kind);
static size_t bx_ls_dired_current_offset(const struct bx_ls_dired_output* output);
static void bx_ls_dired_record_name_range(struct bx_ls_dired_output* output, size_t start, size_t end);
static void bx_ls_dired_record_subdir_range(struct bx_ls_dired_output* output, size_t start, size_t end);
static void bx_ls_dired_write_directory_header(struct bx_ls_dired_output* output, const char* dir_path, bool record_subdir);

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
    options->columns_layout = BX_LS_COLUMNS_VERTICAL;
    options->show_owner = true;
    options->show_group = true;
    options->show_author = false;
    options->sort_entries = true;
    options->sort_mode = BX_LS_SORT_NAME;
    options->escape_names = (variant != BX_LS_VARIANT_LS);
    options->hide_control_chars = false;
    options->color_when = BX_LS_COLOR_NEVER;
    options->indicator_style = BX_LS_INDICATOR_NONE;
    options->time_kind = BX_LS_TIME_MTIME;
    options->time_style = BX_LS_TIME_STYLE_DEFAULT;
    options->quoting_style = BX_LS_QUOTING_DEFAULT;
    options->zero_terminated = false;
    options->width_set = false;
    options->output_width = 0u;
    options->tabsize = 8u;
    options->block_size_set = false;
    options->block_size_divisor = 1u;
    options->block_size_suffix[0] = '\0';
    options->hyperlink_when = BX_LS_HYPERLINK_NEVER;
}

static void bx_ls_options_free(struct bx_ls_options* options) {
    free(options->custom_time_style);
    options->custom_time_style = NULL;
    bx_ls_pattern_list_free(&options->ignore_patterns);
    bx_ls_pattern_list_free(&options->hide_patterns);
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
        options->format = BX_LS_FORMAT_COMMAS;
        return true;
    }

    if (strcmp(text, "across") == 0 || strcmp(text, "horizontal") == 0) {
        options->format = BX_LS_FORMAT_COLUMNS;
        options->columns_layout = BX_LS_COLUMNS_HORIZONTAL;
        return true;
    }

    if (strcmp(text, "vertical") == 0) {
        options->format = BX_LS_FORMAT_COLUMNS;
        options->columns_layout = BX_LS_COLUMNS_VERTICAL;
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

    if (strcmp(text, "version") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_VERSION;
        return true;
    }

    if (strcmp(text, "extension") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_EXTENSION;
        return true;
    }

    if (strcmp(text, "width") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_WIDTH;
        return true;
    }

    if (strcmp(text, "name") == 0) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_NAME;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--sort'", text);
    return false;
}

static bool bx_ls_parse_size_compat(const char* text, size_t* value_out) {
    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return false;
    }

    const char* digits = text;
    while (isspace((unsigned char)*digits)) {
        digits++;
    }

    bool negative = false;
    if (digits[0] == '-') {
        negative = true;
        digits++;
    }
    else if (digits[0] == '+') {
        digits++;
    }

    if (digits[0] == '\0') {
        return false;
    }

    uintmax_t parsed = 0u;
    if (!bx_size_parse_uint(digits, &parsed) || parsed > (uintmax_t)SIZE_MAX) {
        return false;
    }

    size_t value = (size_t)parsed;
    if (negative) {
        value = (size_t)0 - value;
    }

    *value_out = value;
    return true;
}

static bool bx_ls_parse_width_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag, const char* option_name) {
    if (text == NULL) {
        bx_diag(diag, "option '%s' requires an argument", option_name);
        return false;
    }

    size_t parsed = 0u;
    if (!bx_ls_parse_size_compat(text, &parsed)) {
        bx_diag(diag, "invalid argument '%s' for '%s'", text, option_name);
        return false;
    }

    options->width_set = true;
    options->output_width = parsed;
    return true;
}

static bool bx_ls_parse_hyperlink_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    const char* when = (text == NULL) ? "always" : text;

    if (strcmp(when, "always") == 0) {
        options->hyperlink_when = BX_LS_HYPERLINK_ALWAYS;
        return true;
    }
    if (strcmp(when, "auto") == 0) {
        options->hyperlink_when = BX_LS_HYPERLINK_AUTO;
        return true;
    }
    if (strcmp(when, "never") == 0) {
        options->hyperlink_when = BX_LS_HYPERLINK_NEVER;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--hyperlink'", when);
    return false;
}

static bool bx_ls_parse_block_size_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--block-size' requires an argument");
        return false;
    }

    uintmax_t value = 0u;
    const char* suffix = "";
    size_t digits = 0u;
    char with_default_one[32];
    const char* parse_text = text;

    while (text[digits] >= '0' && text[digits] <= '9') {
        digits++;
    }
    if (digits == 0u) {
        if (snprintf(with_default_one, sizeof(with_default_one), "1%s", text) >= (int)sizeof(with_default_one)) {
            bx_diag(diag, "invalid argument '%s' for '--block-size'", text);
            return false;
        }
        parse_text = with_default_one;
        suffix = text;
    }
    else if (text[digits] != '\0') {
        suffix = text + digits;
    }

    if (!bx_size_parse_block_size(parse_text, &value)) {
        bx_diag(diag, "invalid argument '%s' for '--block-size'", text);
        return false;
    }

    options->block_size_set = true;
    options->block_size_divisor = value;
    (void)snprintf(options->block_size_suffix, sizeof(options->block_size_suffix), "%s", suffix);
    return true;
}

static bool bx_ls_parse_tabsize_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag, const char* option_name) {
    if (text == NULL) {
        bx_diag(diag, "option '%s' requires an argument", option_name);
        return false;
    }

    size_t parsed = 0u;
    if (!bx_ls_parse_size_compat(text, &parsed)) {
        bx_diag(diag, "invalid argument '%s' for '%s'", text, option_name);
        return false;
    }

    options->tabsize = parsed;
    return true;
}

static void bx_ls_set_format(struct bx_ls_options* options, enum bx_ls_format format) {
    options->format = format;
}

static bool bx_ls_parse_indicator_style_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--indicator-style' requires an argument");
        return false;
    }

    if (strcmp(text, "none") == 0) {
        options->indicator_style = BX_LS_INDICATOR_NONE;
        return true;
    }
    if (strcmp(text, "slash") == 0) {
        options->indicator_style = BX_LS_INDICATOR_SLASH;
        return true;
    }
    if (strcmp(text, "file-type") == 0) {
        options->indicator_style = BX_LS_INDICATOR_FILE_TYPE;
        return true;
    }
    if (strcmp(text, "classify") == 0) {
        options->indicator_style = BX_LS_INDICATOR_CLASSIFY;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--indicator-style'", text);
    return false;
}

static bool bx_ls_parse_classify_when(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    const char* when = (text == NULL) ? "always" : text;

    if (strcmp(when, "always") == 0 || strcmp(when, "auto") == 0) {
        options->indicator_style = BX_LS_INDICATOR_CLASSIFY;
        return true;
    }
    if (strcmp(when, "never") == 0) {
        options->indicator_style = BX_LS_INDICATOR_NONE;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--classify'", when);
    return false;
}

static bool bx_ls_parse_time_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--time' requires an argument");
        return false;
    }

    if (strcmp(text, "atime") == 0 || strcmp(text, "access") == 0 || strcmp(text, "use") == 0) {
        options->time_kind = BX_LS_TIME_ATIME;
        return true;
    }
    if (strcmp(text, "ctime") == 0 || strcmp(text, "status") == 0) {
        options->time_kind = BX_LS_TIME_CTIME;
        return true;
    }
    if (strcmp(text, "mtime") == 0 || strcmp(text, "modification") == 0) {
        options->time_kind = BX_LS_TIME_MTIME;
        return true;
    }
    if (strcmp(text, "birth") == 0 || strcmp(text, "creation") == 0) {
        options->time_kind = BX_LS_TIME_BIRTH;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--time'", text);
    return false;
}

static bool bx_ls_parse_time_style_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--time-style' requires an argument");
        return false;
    }

    const char* style = text;
    if (strncmp(style, "posix-", 6u) == 0) {
        style += 6u;
    }

    if (strcmp(style, "full-iso") == 0) {
        options->time_style = BX_LS_TIME_STYLE_FULL_ISO;
        free(options->custom_time_style);
        options->custom_time_style = NULL;
        return true;
    }
    if (strcmp(style, "long-iso") == 0) {
        options->time_style = BX_LS_TIME_STYLE_LONG_ISO;
        free(options->custom_time_style);
        options->custom_time_style = NULL;
        return true;
    }
    if (strcmp(style, "iso") == 0) {
        options->time_style = BX_LS_TIME_STYLE_ISO;
        free(options->custom_time_style);
        options->custom_time_style = NULL;
        return true;
    }
    if (strcmp(style, "locale") == 0) {
        options->time_style = BX_LS_TIME_STYLE_LOCALE;
        free(options->custom_time_style);
        options->custom_time_style = NULL;
        return true;
    }
    if (style[0] == '+') {
        options->time_style = BX_LS_TIME_STYLE_CUSTOM;
        free(options->custom_time_style);
        options->custom_time_style = xstrdup(style + 1u);
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--time-style'", text);
    return false;
}

static bool bx_ls_parse_quoting_style_option(const char* text, struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (text == NULL) {
        bx_diag(diag, "option '--quoting-style' requires an argument");
        return false;
    }

    if (strcmp(text, "literal") == 0) {
        options->quoting_style = BX_LS_QUOTING_LITERAL;
        return true;
    }
    if (strcmp(text, "locale") == 0) {
        options->quoting_style = BX_LS_QUOTING_LOCALE;
        return true;
    }
    if (strcmp(text, "shell") == 0) {
        options->quoting_style = BX_LS_QUOTING_SHELL;
        return true;
    }
    if (strcmp(text, "shell-always") == 0) {
        options->quoting_style = BX_LS_QUOTING_SHELL_ALWAYS;
        return true;
    }
    if (strcmp(text, "shell-escape") == 0) {
        options->quoting_style = BX_LS_QUOTING_SHELL_ESCAPE;
        return true;
    }
    if (strcmp(text, "shell-escape-always") == 0) {
        options->quoting_style = BX_LS_QUOTING_SHELL_ESCAPE_ALWAYS;
        return true;
    }
    if (strcmp(text, "c") == 0) {
        options->quoting_style = BX_LS_QUOTING_C;
        return true;
    }
    if (strcmp(text, "escape") == 0) {
        options->quoting_style = BX_LS_QUOTING_ESCAPE;
        return true;
    }

    bx_diag(diag, "invalid argument '%s' for '--quoting-style'", text);
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
        {"file-type", no_argument, NULL, BX_LS_OPT_FILE_TYPE},
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

    bx_args_getopt_reset();
    unsigned long option_order = 0u;
    unsigned long explicit_sort_order = 0u;
    unsigned long short_time_sort_order = 0u;

    while (true) {
        int option_index = 0;
        int c = bx_args_getopt_long(argc, argv, "+1ABCDfFGHI:kLNQRST:UXZabcdghilmnopqrstuvw:x", long_options, &option_index);
        if (c == -1) {
            break;
        }

        switch (c) {
            case '1':
                bx_ls_set_format(options, BX_LS_FORMAT_SINGLE);
                break;
            case 'A':
                options->almost_all = true;
                options->show_all = false;
                break;
            case 'B':
                options->ignore_backups = true;
                break;
            case 'C':
                bx_ls_set_format(options, BX_LS_FORMAT_COLUMNS);
                options->columns_layout = BX_LS_COLUMNS_VERTICAL;
                break;
            case 'D':
                options->dired = true;
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                break;
            case 'G':
                options->show_group = false;
                break;
            case 'H':
                options->dereference_command_line = true;
                break;
            case 'I':
                bx_ls_pattern_list_append(&options->ignore_patterns, optarg);
                break;
            case 'L':
                options->dereference_all = true;
                options->dereference_command_line = true;
                break;
            case 'N':
                options->quoting_style = BX_LS_QUOTING_LITERAL;
                break;
            case 'Q':
                options->quoting_style = BX_LS_QUOTING_C;
                break;
            case 'S':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_SIZE;
                explicit_sort_order = ++option_order;
                break;
            case 'T':
                if (!bx_ls_parse_tabsize_option(optarg, options, diag, "-T")) {
                    return false;
                }
                break;
            case 'X':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_EXTENSION;
                explicit_sort_order = ++option_order;
                break;
            case 'Z':
                options->show_context = true;
                break;
            case 'U':
                options->sort_entries = false;
                explicit_sort_order = ++option_order;
                break;
            case 'a':
                options->show_all = true;
                options->almost_all = false;
                break;
            case 'b':
                options->quoting_style = BX_LS_QUOTING_ESCAPE;
                break;
            case 'c':
                options->time_kind = BX_LS_TIME_CTIME;
                short_time_sort_order = ++option_order;
                break;
            case 'd':
                options->directory_mode = true;
                break;
            case 'F':
                if (!bx_ls_parse_classify_when(optarg, options, diag)) {
                    return false;
                }
                break;
            case 'f':
                options->sort_entries = false;
                options->show_all = true;
                options->almost_all = false;
                explicit_sort_order = ++option_order;
                break;
            case 'g':
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                options->show_owner = false;
                break;
            case 'i':
                options->show_inode = true;
                break;
            case 'k':
                break;
            case 'l':
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                break;
            case 'm':
                bx_ls_set_format(options, BX_LS_FORMAT_COMMAS);
                break;
            case 'n':
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                options->numeric_ids = true;
                break;
            case 'o':
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                options->show_group = false;
                break;
            case 'h':
                options->human_readable = true;
                break;
            case 'p':
                options->indicator_style = BX_LS_INDICATOR_SLASH;
                break;
            case 'q':
                options->hide_control_chars = true;
                break;
            case 'r':
                options->reverse_sort = true;
                break;
            case 'R':
                options->recursive = true;
                break;
            case 's':
                options->show_size_blocks = true;
                break;
            case 't':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_TIME;
                explicit_sort_order = ++option_order;
                break;
            case 'u':
                options->time_kind = BX_LS_TIME_ATIME;
                short_time_sort_order = ++option_order;
                break;
            case 'v':
                options->sort_entries = true;
                options->sort_mode = BX_LS_SORT_VERSION;
                explicit_sort_order = ++option_order;
                break;
            case 'w':
                if (!bx_ls_parse_width_option(optarg, options, diag, "-w")) {
                    return false;
                }
                break;
            case 'x':
                bx_ls_set_format(options, BX_LS_FORMAT_COLUMNS);
                options->columns_layout = BX_LS_COLUMNS_HORIZONTAL;
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
                options->show_author = true;
                break;
            case BX_LS_OPT_BLOCK_SIZE:
                if (!bx_ls_parse_block_size_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_FULL_TIME:
                bx_ls_set_format(options, BX_LS_FORMAT_LONG);
                options->time_style = BX_LS_TIME_STYLE_FULL_ISO;
                break;
            case BX_LS_OPT_GROUP_DIRECTORIES_FIRST:
                break;
            case BX_LS_OPT_DEREFERENCE_CMDLINE_SYMLINK_TO_DIR:
                options->dereference_command_line_symlink_to_dir = true;
                break;
            case BX_LS_OPT_HIDE:
                bx_ls_pattern_list_append(&options->hide_patterns, optarg);
                break;
            case BX_LS_OPT_HYPERLINK:
                if (!bx_ls_parse_hyperlink_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_INDICATOR_STYLE:
                if (!bx_ls_parse_indicator_style_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_FILE_TYPE:
                options->indicator_style = BX_LS_INDICATOR_FILE_TYPE;
                break;
            case BX_LS_OPT_SHOW_CONTROL_CHARS:
                options->hide_control_chars = false;
                break;
            case BX_LS_OPT_QUOTING_STYLE:
                if (!bx_ls_parse_quoting_style_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_SORT:
                if (!bx_ls_parse_sort_option(optarg, options, diag)) {
                    return false;
                }
                explicit_sort_order = ++option_order;
                break;
            case BX_LS_OPT_TIME:
                if (!bx_ls_parse_time_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_TIME_STYLE:
                if (!bx_ls_parse_time_style_option(optarg, options, diag)) {
                    return false;
                }
                break;
            case BX_LS_OPT_ZERO:
                options->zero_terminated = true;
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

    if (short_time_sort_order > explicit_sort_order && options->format != BX_LS_FORMAT_LONG) {
        options->sort_entries = true;
        options->sort_mode = BX_LS_SORT_TIME;
    }

    *first_operand = optind;
    return true;
}

static void bx_ls_pattern_list_append(struct bx_ls_pattern_list* list, char* pattern) {
    if (list->len == list->cap) {
        size_t new_cap = (list->cap == 0) ? 4u : list->cap * 2u;
        list->items = xrealloc(list->items, new_cap * sizeof(*list->items));
        list->cap = new_cap;
    }

    list->items[list->len++] = pattern;
}

static void bx_ls_pattern_list_free(struct bx_ls_pattern_list* list) {
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
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

static void bx_ls_already_listed_dir_error(struct bx_diag_ctx* diag, const char* path) {
    fprintf(stderr, "%s: %s: not listing already-listed directory\n", diag->progname, path);
    if (diag->exit_status < 2) {
        diag->exit_status = 2;
    }
}

static bool bx_ls_entry_load_stat(struct bx_ls_entry* entry, bool follow_for_display) {
    struct stat st;

    if (follow_for_display && stat(entry->full_path, &st) == 0) {
        entry->st = st;
        entry->has_stat = true;
        entry->follow_for_display = true;
        return true;
    }

    if (lstat(entry->full_path, &st) == 0) {
        entry->st = st;
        entry->has_stat = true;
    }
    else {
        entry->has_stat = false;
    }

    entry->follow_for_display = follow_for_display;
    return entry->has_stat;
}

static void bx_ls_dir_stack_push(struct bx_ls_dir_stack* stack, const struct stat* st) {
    if (stack->len == stack->cap) {
        size_t new_cap = (stack->cap == 0u) ? 8u : stack->cap * 2u;
        stack->items = xrealloc(stack->items, new_cap * sizeof(*stack->items));
        stack->cap = new_cap;
    }

    stack->items[stack->len].dev = st->st_dev;
    stack->items[stack->len].ino = st->st_ino;
    stack->len++;
}

static void bx_ls_dir_stack_pop(struct bx_ls_dir_stack* stack) {
    if (stack->len > 0u) {
        stack->len--;
    }
}

static bool bx_ls_dir_stack_contains(const struct bx_ls_dir_stack* stack, const struct stat* st) {
    for (size_t i = 0; i < stack->len; i++) {
        if (stack->items[i].dev == st->st_dev && stack->items[i].ino == st->st_ino) {
            return true;
        }
    }

    return false;
}

static void bx_ls_dir_stack_free(struct bx_ls_dir_stack* stack) {
    free(stack->items);
    stack->items = NULL;
    stack->len = 0u;
    stack->cap = 0u;
}

static void bx_ls_dired_output_append_range(struct bx_ls_dired_range** items,
                                            size_t* len,
                                            size_t* cap,
                                            size_t start,
                                            size_t end) {
    if (*len == *cap) {
        size_t new_cap = (*cap == 0u) ? 8u : (*cap * 2u);
        *items = xrealloc(*items, new_cap * sizeof(**items));
        *cap = new_cap;
    }

    (*items)[*len].start = start;
    (*items)[*len].end = end;
    (*len)++;
}

static bool bx_ls_dired_output_init(struct bx_ls_dired_output* output, struct bx_diag_ctx* diag) {
    memset(output, 0, sizeof(*output));
    output->stream = open_memstream(&output->data, &output->len);
    if (output->stream == NULL) {
        bx_diag(diag, "open_memstream failed: %s", strerror(errno));
        return false;
    }

    return true;
}

static bool bx_ls_dired_output_close(struct bx_ls_dired_output* output, struct bx_diag_ctx* diag) {
    if (output->stream == NULL) {
        return true;
    }

    if (fclose(output->stream) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        output->stream = NULL;
        return false;
    }

    output->stream = NULL;
    return true;
}

static void bx_ls_dired_output_free(struct bx_ls_dired_output* output) {
    if (output->stream != NULL) {
        (void)fclose(output->stream);
        output->stream = NULL;
    }

    free(output->data);
    free(output->name_ranges);
    free(output->subdir_ranges);
    memset(output, 0, sizeof(*output));
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

static const char* bx_ls_name_extension(const char* name, bool* has_extension) {
    const char* last_dot = strrchr(name, '.');
    if (last_dot == NULL || last_dot == name) {
        *has_extension = false;
        return "";
    }

    *has_extension = true;
    return last_dot + 1u;
}

static int bx_ls_compare_names_by_extension(const char* lhs, const char* rhs) {
    bool lhs_has_extension = false;
    bool rhs_has_extension = false;
    const char* lhs_ext = bx_ls_name_extension(lhs, &lhs_has_extension);
    const char* rhs_ext = bx_ls_name_extension(rhs, &rhs_has_extension);

    if (lhs_has_extension != rhs_has_extension) {
        return lhs_has_extension ? 1 : -1;
    }

    if (lhs_has_extension && rhs_has_extension) {
        int cmp = strcmp(lhs_ext, rhs_ext);
        if (cmp != 0) {
            return cmp;
        }
    }

    return strcmp(lhs, rhs);
}

static int bx_ls_compare_names_by_width(const char* lhs, const char* rhs) {
    size_t lhs_len = strlen(lhs);
    size_t rhs_len = strlen(rhs);
    int cmp = bx_ls_compare_intmax((intmax_t)lhs_len, (intmax_t)rhs_len);
    if (cmp != 0) {
        return cmp;
    }

    return strcmp(lhs, rhs);
}

static int bx_ls_compare_names_by_version(const char* lhs, const char* rhs) {
    const unsigned char* a = (const unsigned char*)lhs;
    const unsigned char* b = (const unsigned char*)rhs;

    while (*a != '\0' && *b != '\0') {
        if (isdigit(*a) != 0 && isdigit(*b) != 0) {
            const unsigned char* a_digits = a;
            const unsigned char* b_digits = b;

            while (*a_digits == '0') {
                a_digits++;
            }
            while (*b_digits == '0') {
                b_digits++;
            }

            const unsigned char* a_end = a_digits;
            const unsigned char* b_end = b_digits;
            while (isdigit(*a_end) != 0) {
                a_end++;
            }
            while (isdigit(*b_end) != 0) {
                b_end++;
            }

            size_t a_len = (size_t)(a_end - a_digits);
            size_t b_len = (size_t)(b_end - b_digits);
            if (a_len != b_len) {
                return (a_len < b_len) ? -1 : 1;
            }

            int cmp = memcmp(a_digits, b_digits, a_len);
            if (cmp != 0) {
                return (cmp < 0) ? -1 : 1;
            }

            size_t a_full_len = 0u;
            while (isdigit(a[a_full_len]) != 0) {
                a_full_len++;
            }
            size_t b_full_len = 0u;
            while (isdigit(b[b_full_len]) != 0) {
                b_full_len++;
            }
            if (a_full_len != b_full_len) {
                return (a_full_len < b_full_len) ? 1 : -1;
            }

            a += a_full_len;
            b += b_full_len;
            continue;
        }

        if (*a != *b) {
            return (*a < *b) ? -1 : 1;
        }

        a++;
        b++;
    }

    if (*a == *b) {
        return 0;
    }
    return (*a == '\0') ? -1 : 1;
}

static int bx_ls_entry_compare(const void* lhs, const void* rhs) {
    const struct bx_ls_entry* a = (const struct bx_ls_entry*)lhs;
    const struct bx_ls_entry* b = (const struct bx_ls_entry*)rhs;

    enum bx_ls_sort_mode sort_mode = BX_LS_SORT_NAME;
    enum bx_ls_time_kind time_kind = BX_LS_TIME_MTIME;
    bool reverse = false;
    if (bx_ls_sort_options != NULL) {
        sort_mode = bx_ls_sort_options->sort_mode;
        time_kind = bx_ls_sort_options->time_kind;
        reverse = bx_ls_sort_options->reverse_sort;
    }

    int cmp = 0;
    if (sort_mode == BX_LS_SORT_TIME) {
        if (a->has_stat && b->has_stat) {
            time_t a_sec = bx_ls_selected_time_sec(&a->st, time_kind);
            time_t b_sec = bx_ls_selected_time_sec(&b->st, time_kind);
            long a_nsec = bx_ls_selected_time_nsec(&a->st, time_kind);
            long b_nsec = bx_ls_selected_time_nsec(&b->st, time_kind);

            cmp = bx_ls_compare_intmax((intmax_t)b_sec, (intmax_t)a_sec);
            if (cmp == 0) {
                cmp = bx_ls_compare_intmax((intmax_t)b_nsec, (intmax_t)a_nsec);
            }
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
    else if (sort_mode == BX_LS_SORT_EXTENSION) {
        cmp = bx_ls_compare_names_by_extension(a->name, b->name);
    }
    else if (sort_mode == BX_LS_SORT_VERSION) {
        cmp = bx_ls_compare_names_by_version(a->name, b->name);
    }
    else if (sort_mode == BX_LS_SORT_WIDTH) {
        cmp = bx_ls_compare_names_by_width(a->name, b->name);
    }

    if (cmp == 0) {
        cmp = strcmp(a->name, b->name);
    }

    return reverse ? -cmp : cmp;
}

static int bx_ls_path_compare(const void* lhs, const void* rhs) {
    const char* const* a = (const char* const*)lhs;
    const char* const* b = (const char* const*)rhs;

    enum bx_ls_sort_mode sort_mode = BX_LS_SORT_NAME;
    if (bx_ls_sort_options != NULL) {
        sort_mode = bx_ls_sort_options->sort_mode;
    }

    int cmp = 0;
    if (sort_mode == BX_LS_SORT_EXTENSION) {
        cmp = bx_ls_compare_names_by_extension(*a, *b);
    }
    else if (sort_mode == BX_LS_SORT_VERSION) {
        cmp = bx_ls_compare_names_by_version(*a, *b);
    }
    else if (sort_mode == BX_LS_SORT_WIDTH) {
        cmp = bx_ls_compare_names_by_width(*a, *b);
    }
    else {
        cmp = strcmp(*a, *b);
    }
    if (bx_ls_sort_options != NULL && bx_ls_sort_options->reverse_sort) {
        cmp = -cmp;
    }
    return cmp;
}

static bool bx_ls_name_ends_with_backup_suffix(const char* name) {
    size_t len = strlen(name);
    return len > 0u && name[len - 1u] == '~';
}

static bool bx_ls_name_matches_pattern_list(const char* name, const struct bx_ls_pattern_list* patterns) {
    for (size_t i = 0; i < patterns->len; i++) {
        if (fnmatch(patterns->items[i], name, FNM_PERIOD) == 0) {
            return true;
        }
    }

    return false;
}

static bool bx_ls_name_is_ignored(const char* name, const struct bx_ls_options* options) {
    if (options->ignore_backups && bx_ls_name_ends_with_backup_suffix(name)) {
        return true;
    }

    return bx_ls_name_matches_pattern_list(name, &options->ignore_patterns);
}

static bool bx_ls_name_is_hidden_by_pattern(const char* name, const struct bx_ls_options* options) {
    if (options->show_all || options->almost_all) {
        return false;
    }

    return bx_ls_name_matches_pattern_list(name, &options->hide_patterns);
}

static bool bx_ls_should_include_name(const char* name, const struct bx_ls_options* options) {
    if (bx_ls_name_is_ignored(name, options) || bx_ls_name_is_hidden_by_pattern(name, options)) {
        return false;
    }

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
        (void)bx_ls_entry_load_stat(&entry, options->dereference_all);

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

static time_t bx_ls_selected_time_sec(const struct stat* st, enum bx_ls_time_kind kind) {
    switch (kind) {
        case BX_LS_TIME_CTIME:
            return st->st_ctime;
        case BX_LS_TIME_ATIME:
            return st->st_atime;
        case BX_LS_TIME_BIRTH:
            return st->st_mtime;
        case BX_LS_TIME_MTIME:
        default:
            return st->st_mtime;
    }
}

static long bx_ls_selected_time_nsec(const struct stat* st, enum bx_ls_time_kind kind) {
#if defined(__linux__)
    switch (kind) {
        case BX_LS_TIME_CTIME:
            return st->st_ctim.tv_nsec;
        case BX_LS_TIME_ATIME:
            return st->st_atim.tv_nsec;
        case BX_LS_TIME_BIRTH:
            return st->st_mtim.tv_nsec;
        case BX_LS_TIME_MTIME:
        default:
            return st->st_mtim.tv_nsec;
    }
#else
    (void)st;
    (void)kind;
    return 0;
#endif
}

static bool bx_ls_time_is_recent(time_t timestamp) {
    time_t now = time(NULL);
    if (now == (time_t)-1) {
        return true;
    }

    double delta = difftime(now, timestamp);
    if (delta < 0.0) {
        delta = -delta;
    }

    return !(delta > (365.0 / 2.0) * 24.0 * 60.0 * 60.0 || timestamp > now + 3600);
}

static bool bx_ls_format_with_strftime(const char* fmt, const struct tm* tm_value, char* buffer, size_t buffer_size) {
    return strftime(buffer, buffer_size, fmt, tm_value) != 0u;
}

static void bx_ls_format_custom_timestamp(
    const struct bx_ls_options* options,
    const struct tm* tm_value,
    bool is_recent,
    char* buffer,
    size_t buffer_size) {
    const char* fmt = options->custom_time_style;
    if (fmt == NULL || fmt[0] == '\0') {
        (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
        return;
    }

    const char* newline = strchr(fmt, '\n');
    if (newline != NULL) {
        if (is_recent) {
            fmt = newline + 1;
        }
        else {
            size_t first_len = (size_t)(newline - fmt);
            char* first = xmalloc(first_len + 1u);
            memcpy(first, fmt, first_len);
            first[first_len] = '\0';
            if (!bx_ls_format_with_strftime(first, tm_value, buffer, buffer_size)) {
                (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
            }
            free(first);
            return;
        }
    }

    if (!bx_ls_format_with_strftime(fmt, tm_value, buffer, buffer_size)) {
        (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
    }
}

static void bx_ls_format_timestamp(
    const struct bx_ls_options* options,
    time_t timestamp,
    long nsec,
    char* buffer,
    size_t buffer_size) {
    if (buffer_size == 0u) {
        return;
    }

    struct tm tm_value;
    if (localtime_r(&timestamp, &tm_value) == NULL) {
        (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
        return;
    }

    bool is_recent = bx_ls_time_is_recent(timestamp);

    switch (options->time_style) {
        case BX_LS_TIME_STYLE_FULL_ISO: {
            char datetime[32];
            char zone[16];
            if (!bx_ls_format_with_strftime("%Y-%m-%d %H:%M:%S", &tm_value, datetime, sizeof(datetime))
                || !bx_ls_format_with_strftime("%z", &tm_value, zone, sizeof(zone))) {
                (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
                return;
            }
            (void)snprintf(buffer, buffer_size, "%s.%09ld %s", datetime, nsec, zone);
            return;
        }
        case BX_LS_TIME_STYLE_LONG_ISO:
            if (!bx_ls_format_with_strftime("%Y-%m-%d %H:%M", &tm_value, buffer, buffer_size)) {
                (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
            }
            return;
        case BX_LS_TIME_STYLE_ISO:
            if (!bx_ls_format_with_strftime(is_recent ? "%m-%d %H:%M" : "%Y-%m-%d ", &tm_value, buffer, buffer_size)) {
                (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
            }
            return;
        case BX_LS_TIME_STYLE_CUSTOM:
            bx_ls_format_custom_timestamp(options, &tm_value, is_recent, buffer, buffer_size);
            return;
        case BX_LS_TIME_STYLE_LOCALE:
        case BX_LS_TIME_STYLE_DEFAULT:
        default:
            if (!bx_ls_format_with_strftime(
                    is_recent ? "%b %e %H:%M" : "%b %e  %Y",
                    &tm_value,
                    buffer,
                    buffer_size)) {
                (void)snprintf(buffer, buffer_size, "??? ?? ??:??");
            }
            return;
    }
}

static size_t bx_ls_escape_append_octal(char* out, size_t out_pos, unsigned char ch) {
    out[out_pos++] = '\\';
    out[out_pos++] = (char)('0' + ((ch >> 6) & 7u));
    out[out_pos++] = (char)('0' + ((ch >> 3) & 7u));
    out[out_pos++] = (char)('0' + (ch & 7u));
    return out_pos;
}

static bool bx_ls_is_nongraphic(unsigned char ch) {
    return isprint(ch) == 0;
}

static enum bx_ls_quoting_style bx_ls_effective_quoting_style(const struct bx_ls_options* options) {
    if (options->quoting_style != BX_LS_QUOTING_DEFAULT) {
        return options->quoting_style;
    }

    return options->escape_names ? BX_LS_QUOTING_ESCAPE : BX_LS_QUOTING_LITERAL;
}

static const char* bx_ls_quoting_style_name(const struct bx_ls_options* options) {
    switch (bx_ls_effective_quoting_style(options)) {
        case BX_LS_QUOTING_LITERAL:
            return "literal";
        case BX_LS_QUOTING_LOCALE:
            return "locale";
        case BX_LS_QUOTING_SHELL:
            return "shell";
        case BX_LS_QUOTING_SHELL_ALWAYS:
            return "shell-always";
        case BX_LS_QUOTING_SHELL_ESCAPE:
            return "shell-escape";
        case BX_LS_QUOTING_SHELL_ESCAPE_ALWAYS:
            return "shell-escape-always";
        case BX_LS_QUOTING_C:
            return "c";
        case BX_LS_QUOTING_ESCAPE:
            return "escape";
        case BX_LS_QUOTING_DEFAULT:
        default:
            return "literal";
    }
}

static size_t bx_ls_append_escape_char(
    char* out,
    size_t out_pos,
    unsigned char ch,
    bool escape_space,
    bool escape_double_quote,
    bool escape_single_quote) {
    switch (ch) {
        case ' ':
            if (escape_space) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = ' ';
            break;
        case '\\':
            out[out_pos++] = '\\';
            out[out_pos++] = '\\';
            break;
        case '"':
            if (escape_double_quote) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = '"';
            break;
        case '\'':
            if (escape_single_quote) {
                out[out_pos++] = '\\';
            }
            out[out_pos++] = '\'';
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
            if (!bx_ls_is_nongraphic(ch)) {
                out[out_pos++] = (char)ch;
            }
            else {
                out_pos = bx_ls_escape_append_octal(out, out_pos, ch);
            }
            break;
    }

    return out_pos;
}

static char* bx_ls_render_literal_name(const char* name, bool hide_control_chars) {
    size_t len = strlen(name);
    char* out = xmalloc(len + 1u);
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (hide_control_chars && bx_ls_is_nongraphic(ch)) {
            out[out_pos++] = '?';
        }
        else {
            out[out_pos++] = (char)ch;
        }
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_ls_render_escape_style_name(const char* name) {
    size_t len = strlen(name);
    char* out = xmalloc((len * 4u) + 1u);
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i++) {
        out_pos = bx_ls_append_escape_char(out, out_pos, (unsigned char)name[i], true, false, false);
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_ls_render_c_style_name(const char* name) {
    size_t len = strlen(name);
    char* out = xmalloc((len * 4u) + 3u);
    size_t out_pos = 0;
    out[out_pos++] = '"';

    for (size_t i = 0; i < len; i++) {
        out_pos = bx_ls_append_escape_char(out, out_pos, (unsigned char)name[i], false, true, false);
    }

    out[out_pos++] = '"';
    out[out_pos] = '\0';
    return out;
}

static char* bx_ls_render_locale_name(const char* name) {
    size_t len = strlen(name);
    char* out = xmalloc((len * 4u) + 3u);
    size_t out_pos = 0;
    out[out_pos++] = '\'';

    for (size_t i = 0; i < len; i++) {
        out_pos = bx_ls_append_escape_char(out, out_pos, (unsigned char)name[i], false, false, true);
    }

    out[out_pos++] = '\'';
    out[out_pos] = '\0';
    return out;
}

static bool bx_ls_shell_char_is_safe(unsigned char ch) {
    return isalnum(ch) != 0
        || ch == '-'
        || ch == '_'
        || ch == '.'
        || ch == '/'
        || ch == '~';
}

static bool bx_ls_name_has_nongraphic(const char* name) {
    for (size_t i = 0; name[i] != '\0'; i++) {
        if (bx_ls_is_nongraphic((unsigned char)name[i])) {
            return true;
        }
    }

    return false;
}

static size_t bx_ls_append_shell_double_quoted_segment(char* out, size_t out_pos, const char* text, size_t len) {
    out[out_pos++] = '"';
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            out[out_pos++] = '\\';
        }
        out[out_pos++] = (char)ch;
    }
    out[out_pos++] = '"';
    return out_pos;
}

static size_t bx_ls_append_shell_single_quoted_segment(char* out, size_t out_pos, const char* text, size_t len) {
    out[out_pos++] = '\'';
    memcpy(out + out_pos, text, len);
    out_pos += len;
    out[out_pos++] = '\'';
    return out_pos;
}

static size_t bx_ls_append_shell_quoted_segment(char* out, size_t out_pos, const char* text, size_t len, bool always_quote) {
    bool needs_quotes = always_quote;
    if (!needs_quotes) {
        for (size_t i = 0; i < len; i++) {
            if (!bx_ls_shell_char_is_safe((unsigned char)text[i])) {
                needs_quotes = true;
                break;
            }
        }
    }

    if (!needs_quotes) {
        memcpy(out + out_pos, text, len);
        out_pos += len;
        return out_pos;
    }

    if (memchr(text, '\'', len) == NULL) {
        return bx_ls_append_shell_single_quoted_segment(out, out_pos, text, len);
    }

    return bx_ls_append_shell_double_quoted_segment(out, out_pos, text, len);
}

static char* bx_ls_render_shell_style_name(const char* name, bool always_quote) {
    size_t len = strlen(name);
    char* out = xmalloc((len * 4u) + 3u);
    size_t out_pos = 0;

    out_pos = bx_ls_append_shell_quoted_segment(out, out_pos, name, len, always_quote);
    out[out_pos] = '\0';
    return out;
}

static size_t bx_ls_append_shell_escape_fragment(char* out, size_t out_pos, unsigned char ch) {
    memcpy(out + out_pos, "$'", 2u);
    out_pos += 2u;
    out_pos = bx_ls_append_escape_char(out, out_pos, ch, false, false, false);
    out[out_pos++] = '\'';
    return out_pos;
}

static char* bx_ls_render_shell_escape_style_name(const char* name, bool always_quote) {
    if (!bx_ls_name_has_nongraphic(name)) {
        return bx_ls_render_shell_style_name(name, always_quote);
    }

    size_t len = strlen(name);
    char* out = xmalloc((len * 8u) + 8u);
    size_t out_pos = 0;
    size_t segment_start = 0u;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (!bx_ls_is_nongraphic(ch)) {
            continue;
        }

        if (i > segment_start) {
            out_pos = bx_ls_append_shell_quoted_segment(out, out_pos, name + segment_start, i - segment_start, true);
        }
        out_pos = bx_ls_append_shell_escape_fragment(out, out_pos, ch);
        segment_start = i + 1u;
    }

    if (segment_start < len) {
        out_pos = bx_ls_append_shell_quoted_segment(out, out_pos, name + segment_start, len - segment_start, true);
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_ls_render_name(const char* name, const struct bx_ls_options* options) {
    switch (bx_ls_effective_quoting_style(options)) {
        case BX_LS_QUOTING_LITERAL:
            return bx_ls_render_literal_name(name, options->hide_control_chars);
        case BX_LS_QUOTING_LOCALE:
            return bx_ls_render_locale_name(name);
        case BX_LS_QUOTING_SHELL:
            return bx_ls_render_shell_style_name(name, false);
        case BX_LS_QUOTING_SHELL_ALWAYS:
            return bx_ls_render_shell_style_name(name, true);
        case BX_LS_QUOTING_SHELL_ESCAPE:
            return bx_ls_render_shell_escape_style_name(name, false);
        case BX_LS_QUOTING_SHELL_ESCAPE_ALWAYS:
            return bx_ls_render_shell_escape_style_name(name, true);
        case BX_LS_QUOTING_C:
            return bx_ls_render_c_style_name(name);
        case BX_LS_QUOTING_ESCAPE:
            return bx_ls_render_escape_style_name(name);
        case BX_LS_QUOTING_DEFAULT:
        default:
            return bx_ls_render_literal_name(name, options->hide_control_chars);
    }
}

static char bx_ls_indicator_char(mode_t mode, const struct bx_ls_options* options) {
    switch (options->indicator_style) {
        case BX_LS_INDICATOR_SLASH:
            return S_ISDIR(mode) ? '/' : '\0';
        case BX_LS_INDICATOR_FILE_TYPE:
        case BX_LS_INDICATOR_CLASSIFY:
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
            if (options->indicator_style == BX_LS_INDICATOR_CLASSIFY
                && S_ISREG(mode)
                && (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0) {
                return '*';
            }
            return '\0';
        case BX_LS_INDICATOR_NONE:
        default:
            return '\0';
    }
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

static bool bx_ls_hyperlink_enabled(const struct bx_ls_options* options) {
    switch (options->hyperlink_when) {
        case BX_LS_HYPERLINK_ALWAYS:
            return true;
        case BX_LS_HYPERLINK_AUTO:
            return isatty(STDOUT_FILENO);
        case BX_LS_HYPERLINK_NEVER:
        default:
            return false;
    }
}

static char* bx_ls_absolute_path_for_hyperlink(const char* path) {
    char* resolved = realpath(path, NULL);
    if (resolved != NULL) {
        return resolved;
    }

    if (path[0] == '/') {
        return xstrdup(path);
    }

    char* cwd = getcwd(NULL, 0u);
    if (cwd == NULL) {
        return xstrdup(path);
    }

    char* joined = bx_ls_join_path(cwd, path);
    free(cwd);
    return joined;
}

static char* bx_ls_escape_uri_path(const char* path) {
    size_t len = strlen(path);
    char* out = xmalloc((len * 3u) + 1u);
    size_t out_pos = 0;

    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)path[i];
        if (isalnum(ch) != 0 || ch == '/' || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out[out_pos++] = (char)ch;
        }
        else {
            static const char hex[] = "0123456789ABCDEF";
            out[out_pos++] = '%';
            out[out_pos++] = hex[(ch >> 4) & 0x0fu];
            out[out_pos++] = hex[ch & 0x0fu];
        }
    }

    out[out_pos] = '\0';
    return out;
}

static char* bx_ls_hyperlink_target_for_entry(const struct bx_ls_entry* entry) {
    return bx_ls_absolute_path_for_hyperlink(entry->full_path);
}

static char* bx_ls_hyperlink_wrap(const char* text, const char* target_path, const struct bx_ls_options* options) {
    if (!bx_ls_hyperlink_enabled(options) || target_path == NULL) {
        return xstrdup(text);
    }

    char host[256];
    if (gethostname(host, sizeof(host)) != 0) {
        host[0] = '\0';
    }
    host[sizeof(host) - 1u] = '\0';

    char* uri_path = bx_ls_escape_uri_path(target_path);
    size_t text_len = strlen(text);
    size_t host_len = strlen(host);
    size_t uri_len = strlen(uri_path);
    size_t total = 12u + host_len + uri_len + 2u + text_len + 7u;
    char* out = xmalloc(total + 1u);
    size_t pos = 0u;

    pos += (size_t)snprintf(out + pos, total + 1u - pos, "\033]8;;file://%s%s\033\\", host, uri_path);
    memcpy(out + pos, text, text_len);
    pos += text_len;
    pos += (size_t)snprintf(out + pos, total + 1u - pos, "\033]8;;\033\\");
    out[pos] = '\0';

    free(uri_path);
    return out;
}

static uintmax_t bx_ls_ceil_div_uintmax(uintmax_t value, uintmax_t divisor) {
    if (divisor == 0u) {
        return value;
    }
    return (value + divisor - 1u) / divisor;
}

static uintmax_t bx_ls_allocated_bytes(const struct stat* st) {
    if (st->st_blocks <= 0) {
        return 0u;
    }
    return (uintmax_t)st->st_blocks * 512u;
}

static void bx_ls_format_human_bytes(uintmax_t size, bool si_units, char buffer[32]) {
    const uintmax_t base = si_units ? 1000u : 1024u;
    if (size < base) {
        (void)snprintf(buffer, 32u, "%" PRIuMAX, size);
        return;
    }

    static const char* units_1024[] = {"", "K", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    static const char* units_1000[] = {"", "k", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q"};
    const char* const* units = si_units ? units_1000 : units_1024;
    const size_t max_unit = (sizeof(units_1024) / sizeof(units_1024[0])) - 1u;

    size_t unit = 0;
    uintmax_t divisor = 1u;

    while (size >= divisor * base && unit < max_unit) {
        divisor *= base;
        unit++;
    }

    if (size < divisor * 10u) {
        uintmax_t tenths = bx_ls_ceil_div_uintmax(size * 10u, divisor);
        (void)snprintf(buffer, 32u, "%" PRIuMAX ".%" PRIuMAX "%s", tenths / 10u, tenths % 10u, units[unit]);
    }
    else {
        (void)snprintf(buffer, 32u, "%" PRIuMAX "%s", bx_ls_ceil_div_uintmax(size, divisor), units[unit]);
    }
}

static void bx_ls_format_scaled_exact_or_human(uintmax_t size, const struct bx_ls_options* options, char buffer[32]) {
    if (options->block_size_set) {
        uintmax_t scaled = bx_ls_ceil_div_uintmax(size, options->block_size_divisor);
        (void)snprintf(buffer, 32u, "%" PRIuMAX "%s", scaled, options->block_size_suffix);
        return;
    }

    if (options->human_readable) {
        bx_ls_format_human_bytes(size, options->si_units, buffer);
        return;
    }

    (void)snprintf(buffer, 32u, "%" PRIuMAX, size);
}

static void bx_ls_format_file_size(uintmax_t size, const struct bx_ls_options* options, char buffer[32]) {
    if (!options->block_size_set && !options->human_readable) {
        (void)snprintf(buffer, 32u, "%" PRIuMAX, size);
        return;
    }

    bx_ls_format_scaled_exact_or_human(size, options, buffer);
}

static void bx_ls_format_block_count(uintmax_t allocated_bytes, const struct bx_ls_options* options, char buffer[32]) {
    if (options->block_size_set || options->human_readable) {
        bx_ls_format_scaled_exact_or_human(allocated_bytes, options, buffer);
        return;
    }

    (void)snprintf(buffer, 32u, "%" PRIuMAX, bx_ls_ceil_div_uintmax(allocated_bytes, 1024u));
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
    if (!entry->follow_for_display) {
        if (entry->has_stat) {
            *st = entry->st;
            return true;
        }

        if (lstat(entry->full_path, st) == 0) {
            return true;
        }
    }
    else {
        if (stat(entry->full_path, st) == 0) {
            return true;
        }
        if (entry->has_stat) {
            *st = entry->st;
            return true;
        }
        if (lstat(entry->full_path, st) == 0) {
            return true;
        }
    }

    if (emit_error) {
        bx_ls_perror_path(diag, entry->full_path, 1);
    }
    return false;
}

static void bx_ls_put_line_terminator(const struct bx_ls_options* options) {
    (void)fputc(options->zero_terminated ? '\0' : '\n', stdout);
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
    widths->blocks = options->show_size_blocks ? 1u : 0u;
    widths->nlink = 1u;
    widths->user = options->show_owner ? 1u : 0u;
    widths->group = options->show_group ? 1u : 0u;
    widths->author = options->show_author ? 1u : 0u;
    widths->context = options->show_context ? 1u : 0u;
    widths->size = 1u;
}

static void bx_ls_short_widths_init(struct bx_ls_short_widths* widths, const struct bx_ls_options* options) {
    widths->inode = options->show_inode ? 1u : 0u;
    widths->blocks = options->show_size_blocks ? 1u : 0u;
}

static void bx_ls_compute_short_widths(
    const struct bx_ls_entry_list* entries,
    const struct bx_ls_options* options,
    struct bx_diag_ctx* diag,
    struct bx_ls_short_widths* widths) {
    bx_ls_short_widths_init(widths, options);

    if (!options->show_inode && !options->show_size_blocks) {
        return;
    }

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

        if (options->show_size_blocks) {
            char blocks_text[32];
            bx_ls_format_block_count(bx_ls_allocated_bytes(&st), options, blocks_text);
            size_t blocks_width = strlen(blocks_text);
            if (blocks_width > widths->blocks) {
                widths->blocks = blocks_width;
            }
        }
    }
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

        if (options->show_size_blocks) {
            char blocks_text[32];
            bx_ls_format_block_count(bx_ls_allocated_bytes(&st), options, blocks_text);
            size_t blocks_width = strlen(blocks_text);
            if (blocks_width > widths->blocks) {
                widths->blocks = blocks_width;
            }
        }

        size_t nlink_width = bx_ls_uintmax_width((uintmax_t)st.st_nlink);
        if (nlink_width > widths->nlink) {
            widths->nlink = nlink_width;
        }

        if (options->show_owner || options->show_author) {
            char user_numeric[32];
            const char* user_name = bx_ls_user_name(st.st_uid, options->numeric_ids, user_numeric);
            size_t user_width = strlen(user_name);
            if (options->show_owner && user_width > widths->user) {
                widths->user = user_width;
            }
            if (options->show_author && user_width > widths->author) {
                widths->author = user_width;
            }
        }

        if (options->show_group) {
            char group_numeric[32];
            const char* group_name = bx_ls_group_name(st.st_gid, options->numeric_ids, group_numeric);
            size_t group_width = strlen(group_name);
            if (group_width > widths->group) {
                widths->group = group_width;
            }
        }

        char size_text[32];
        bx_ls_format_file_size((uintmax_t)st.st_size, options, size_text);
        size_t size_width = strlen(size_text);
        if (size_width > widths->size) {
            widths->size = size_width;
        }
    }
}

static bool bx_ls_build_short_cell(const struct bx_ls_entry* entry,
                                   const struct bx_ls_options* options,
                                   const struct bx_ls_short_widths* widths,
                                   struct bx_diag_ctx* diag,
                                   char** out_cell) {
    struct stat st;
    bool have_stat = bx_ls_entry_stat(entry, &st, diag, options->show_inode || options->show_size_blocks);

    char* name = bx_ls_render_name(entry->name, options);
    if (have_stat) {
        name = bx_ls_append_indicator(name, bx_ls_indicator_char(st.st_mode, options));
        char* colored_name = bx_ls_colorize_name(name, entry, &st, options);
        free(name);
        name = colored_name;
    }
    char* hyperlink_target = bx_ls_hyperlink_target_for_entry(entry);
    char* hyperlinked_name = bx_ls_hyperlink_wrap(name, hyperlink_target, options);
    free(hyperlink_target);
    free(name);
    name = hyperlinked_name;

    if (!options->show_inode && !options->show_size_blocks && !options->show_context) {
        *out_cell = name;
        return true;
    }

    if ((options->show_inode || options->show_size_blocks) && !have_stat) {
        free(name);
        return false;
    }

    char prefix[128];
    size_t prefix_len = 0u;
    prefix[0] = '\0';
    if (options->show_inode) {
        int width = (widths != NULL) ? (int)widths->inode : 0;
        prefix_len += (size_t)snprintf(prefix + prefix_len,
                                       sizeof(prefix) - prefix_len,
                                       "%*" PRIuMAX " ",
                                       width,
                                       (uintmax_t)st.st_ino);
    }
    if (options->show_size_blocks) {
        char blocks_text[32];
        bx_ls_format_block_count(bx_ls_allocated_bytes(&st), options, blocks_text);
        int width = (widths != NULL) ? (int)widths->blocks : 0;
        prefix_len += (size_t)snprintf(prefix + prefix_len,
                                       sizeof(prefix) - prefix_len,
                                       "%*s ",
                                       width,
                                       blocks_text);
    }
    if (options->show_context) {
        prefix_len += (size_t)snprintf(prefix + prefix_len, sizeof(prefix) - prefix_len, "? ");
    }

    size_t name_len = strlen(name);
    char* combined = xmalloc(prefix_len + name_len + 1u);
    memcpy(combined, prefix, prefix_len);
    memcpy(combined + prefix_len, name, name_len + 1u);
    free(name);
    *out_cell = combined;
    return true;
}

static size_t bx_ls_output_width(const struct bx_ls_options* options) {
    if (options->width_set) {
        return (options->output_width == 0u) ? SIZE_MAX : options->output_width;
    }

    const char* columns_env = getenv("COLUMNS");
    if (columns_env != NULL && columns_env[0] != '\0') {
        size_t parsed = 0u;
        if (bx_ls_parse_size_compat(columns_env, &parsed) && parsed > 0u) {
            return parsed;
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
        if ((unsigned char)text[i] == 0x1b) {
            if (text[i + 1u] == '[') {
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

            if (text[i + 1u] == ']') {
                i += 2u;
                while (text[i] != '\0') {
                    if ((unsigned char)text[i] == '\a') {
                        i++;
                        break;
                    }
                    if ((unsigned char)text[i] == 0x1b && text[i + 1u] == '\\') {
                        i += 2u;
                        break;
                    }
                    i++;
                }
                continue;
            }
        }

        i++;
        width++;
    }

    return width;
}

static size_t bx_ls_columns_index(
    const struct bx_ls_options* options,
    size_t row,
    size_t col,
    size_t row_count,
    size_t column_count) {
    if (options->columns_layout == BX_LS_COLUMNS_HORIZONTAL) {
        return row * column_count + col;
    }

    return col * row_count + row;
}

static size_t bx_ls_column_width_for(
    const char* const* cells,
    size_t cell_count,
    const struct bx_ls_options* options,
    size_t row_count,
    size_t column_count,
    size_t col,
    bool* has_values) {
    size_t width = 0u;
    bool any = false;

    for (size_t row = 0; row < row_count; row++) {
        size_t idx = bx_ls_columns_index(options, row, col, row_count, column_count);
        if (idx >= cell_count) {
            continue;
        }

        any = true;
        size_t used = bx_ls_display_width(cells[idx]);
        if (used > width) {
            width = used;
        }
    }

    if (has_values != NULL) {
        *has_values = any;
    }
    return width;
}

static size_t bx_ls_min_padding_bytes(size_t current_col, size_t target_col, size_t tabsize) {
    if (current_col >= target_col) {
        return 0u;
    }

    size_t best = target_col - current_col;
    if (tabsize > 1u) {
        size_t next_tab = ((current_col / tabsize) + 1u) * tabsize;
        if (next_tab <= target_col && next_tab > current_col) {
            size_t with_tab = 1u + bx_ls_min_padding_bytes(next_tab, target_col, tabsize);
            if (with_tab < best) {
                best = with_tab;
            }
        }
    }

    return best;
}

static void bx_ls_print_column_padding(size_t current_col, size_t target_col, size_t tabsize) {
    while (current_col < target_col) {
        if (tabsize > 1u) {
            size_t next_tab = ((current_col / tabsize) + 1u) * tabsize;
            size_t spaces_only = target_col - current_col;
            size_t with_tab = (next_tab <= target_col && next_tab > current_col)
                ? 1u + bx_ls_min_padding_bytes(next_tab, target_col, tabsize)
                : SIZE_MAX;
            if (with_tab < spaces_only) {
                (void)fputc('\t', stdout);
                current_col = next_tab;
                continue;
            }
        }

        (void)fputc(' ', stdout);
        current_col++;
    }
}

static void bx_ls_print_entries_single(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    struct bx_ls_short_widths widths;
    bx_ls_compute_short_widths(entries, options, diag, &widths);

    for (size_t i = 0; i < entries->len; i++) {
        char* cell = NULL;
        if (!bx_ls_build_short_cell(&entries->items[i], options, &widths, diag, &cell)) {
            continue;
        }

        (void)fputs(cell, stdout);
        bx_ls_put_line_terminator(options);
        free(cell);
    }
}

static void bx_ls_print_entries_commas(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    size_t term_width = bx_ls_output_width(options);
    bool first_on_line = true;
    size_t current_width = 0u;
    struct bx_ls_short_widths widths;
    bx_ls_compute_short_widths(entries, options, diag, &widths);

    for (size_t i = 0; i < entries->len; i++) {
        char* cell = NULL;
        if (!bx_ls_build_short_cell(&entries->items[i], options, &widths, diag, &cell)) {
            continue;
        }

        bool is_last = (i + 1u == entries->len);
        size_t cell_width = bx_ls_display_width(cell);
        size_t item_width = cell_width + (is_last ? 0u : 1u);

        if (first_on_line) {
            (void)fputs(cell, stdout);
            if (!is_last) {
                (void)fputc(',', stdout);
            }
            current_width = item_width;
            first_on_line = false;
        }
        else if (current_width + 1u + item_width <= term_width) {
            (void)fputc(' ', stdout);
            (void)fputs(cell, stdout);
            if (!is_last) {
                (void)fputc(',', stdout);
            }
            current_width += 1u + item_width;
        }
        else {
            bx_ls_put_line_terminator(options);
            (void)fputs(cell, stdout);
            if (!is_last) {
                (void)fputc(',', stdout);
            }
            current_width = item_width;
        }

        free(cell);
    }

    if (!first_on_line || entries->len == 0u) {
        bx_ls_put_line_terminator(options);
    }
}

static void bx_ls_print_entries_columns(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag) {
    if (entries->len == 0) {
        return;
    }

    struct bx_ls_short_widths widths;
    bx_ls_compute_short_widths(entries, options, diag, &widths);

    char** cells = xmalloc(entries->len * sizeof(*cells));
    size_t cell_count = 0;

    for (size_t i = 0; i < entries->len; i++) {
        char* cell = NULL;
        if (!bx_ls_build_short_cell(&entries->items[i], options, &widths, diag, &cell)) {
            continue;
        }

        cells[cell_count++] = cell;
    }

    if (cell_count == 0) {
        free(cells);
        return;
    }

    size_t term_width = bx_ls_output_width(options);
    size_t column_count = 1u;

    for (size_t candidate = 1u; candidate <= cell_count; candidate++) {
        size_t candidate_row_count = (cell_count + candidate - 1u) / candidate;
        size_t total_width = 0u;
        size_t used_columns = 0u;

        for (size_t col = 0; col < candidate; col++) {
            bool has_values = false;
            size_t width = bx_ls_column_width_for(
                (const char* const*)cells,
                cell_count,
                options,
                candidate_row_count,
                candidate,
                col,
                &has_values);
            if (!has_values) {
                continue;
            }

            if (used_columns > 0u) {
                total_width += 2u;
            }
            total_width += width;
            used_columns++;
        }

        if (total_width <= term_width) {
            column_count = candidate;
        }
    }

    size_t row_count = (cell_count + column_count - 1u) / column_count;
    size_t* column_widths = xmalloc(column_count * sizeof(*column_widths));
    size_t* column_starts = xmalloc(column_count * sizeof(*column_starts));
    for (size_t col = 0; col < column_count; col++) {
        column_widths[col] = bx_ls_column_width_for(
            (const char* const*)cells,
            cell_count,
            options,
            row_count,
            column_count,
            col,
            NULL);
    }
    if (column_count > 0u) {
        column_starts[0] = 0u;
        for (size_t col = 1u; col < column_count; col++) {
            column_starts[col] = column_starts[col - 1u] + column_widths[col - 1u] + 2u;
        }
    }

    for (size_t row = 0; row < row_count; row++) {
        for (size_t col = 0; col < column_count; col++) {
            size_t idx = bx_ls_columns_index(options, row, col, row_count, column_count);
            if (idx >= cell_count) {
                continue;
            }

            (void)fputs(cells[idx], stdout);
            size_t current_col = column_starts[col] + bx_ls_display_width(cells[idx]);

            size_t next_col = col + 1u;
            while (next_col < column_count) {
                size_t next_idx = bx_ls_columns_index(options, row, next_col, row_count, column_count);
                if (next_idx < cell_count) {
                    break;
                }
                next_col++;
            }

            if (next_col < column_count) {
                bx_ls_print_column_padding(current_col, column_starts[next_col], options->tabsize);
            }
        }
        bx_ls_put_line_terminator(options);
    }

    for (size_t i = 0; i < cell_count; i++) {
        free(cells[i]);
    }
    free(column_starts);
    free(column_widths);
    free(cells);
}

static bool bx_ls_format_long_entry_line(
    const struct bx_ls_entry* entry,
    const struct bx_ls_options* options,
    const struct bx_ls_long_widths* widths,
    struct bx_diag_ctx* diag,
    char** line_out,
    size_t* name_start_out,
    size_t* name_end_out) {
    char* line = NULL;
    size_t line_len = 0u;
    FILE* stream = open_memstream(&line, &line_len);
    if (stream == NULL) {
        bx_diag(diag, "open_memstream failed: %s", strerror(errno));
        return false;
    }

    struct stat st;
    if (!bx_ls_entry_stat(entry, &st, diag, true)) {
        (void)fclose(stream);
        free(line);
        return false;
    }

    if (name_start_out != NULL) {
        *name_start_out = 0u;
    }
    if (name_end_out != NULL) {
        *name_end_out = 0u;
    }

    char mode[11];
    bx_ls_mode_to_string(st.st_mode, mode);

    char user_numeric[32];
    char group_numeric[32];
    const char* user_name = bx_ls_user_name(st.st_uid, options->numeric_ids, user_numeric);
    const char* group_name = bx_ls_group_name(st.st_gid, options->numeric_ids, group_numeric);
    const char* author_name = user_name;

    char timestamp[64];
    bx_ls_format_timestamp(
        options,
        bx_ls_selected_time_sec(&st, options->time_kind),
        bx_ls_selected_time_nsec(&st, options->time_kind),
        timestamp,
        sizeof(timestamp));
    char size[32];
    bx_ls_format_file_size((uintmax_t)st.st_size, options, size);

    char* display_name = bx_ls_render_name(entry->name, options);
    display_name = bx_ls_append_indicator(display_name, bx_ls_indicator_char(st.st_mode, options));
    char* colored_name = bx_ls_colorize_name(display_name, entry, &st, options);
    free(display_name);
    display_name = colored_name;
    char* hyperlink_target = bx_ls_hyperlink_target_for_entry(entry);
    char* hyperlinked_name = bx_ls_hyperlink_wrap(display_name, hyperlink_target, options);
    free(display_name);
    display_name = hyperlinked_name;

    char* symlink_display = NULL;
    if (S_ISLNK(st.st_mode)) {
        char* symlink_target = bx_ls_readlink_target(entry->full_path);
        if (symlink_target == NULL) {
            bx_ls_perror_path(diag, entry->full_path, 1);
        }
        else {
            symlink_display = bx_ls_render_name(symlink_target, options);
            char* hyperlinked_target = bx_ls_hyperlink_wrap(symlink_display, hyperlink_target, options);
            free(symlink_display);
            symlink_display = hyperlinked_target;
            free(symlink_target);
        }
    }

    if (options->show_inode) {
        (void)fprintf(stream, "%*" PRIuMAX " ", (int)widths->inode, (uintmax_t)st.st_ino);
    }

    if (options->show_size_blocks) {
        char blocks_text[32];
        bx_ls_format_block_count(bx_ls_allocated_bytes(&st), options, blocks_text);
        (void)fprintf(stream, "%*s ", (int)widths->blocks, blocks_text);
    }

    (void)fprintf(stream, "%s %*" PRIuMAX, mode, (int)widths->nlink, (uintmax_t)st.st_nlink);

    if (options->show_owner) {
        (void)fprintf(stream, " %-*s", (int)widths->user, user_name);
    }
    if (options->show_group) {
        (void)fprintf(stream, " %-*s", (int)widths->group, group_name);
    }
    if (options->show_author) {
        (void)fprintf(stream, " %-*s", (int)widths->author, author_name);
    }
    if (options->show_context) {
        (void)fprintf(stream, " %-*s", (int)widths->context, "?");
    }

    (void)fprintf(stream, " %*s %s ", (int)widths->size, size, timestamp);
    if (name_start_out != NULL) {
        long pos = ftell(stream);
        if (pos >= 0) {
            *name_start_out = (size_t)pos;
        }
    }
    (void)fputs(display_name, stream);
    if (name_end_out != NULL) {
        long pos = ftell(stream);
        if (pos >= 0) {
            *name_end_out = (size_t)pos;
        }
    }

    if (symlink_display != NULL) {
        (void)fprintf(stream, " -> %s", symlink_display);
    }

    free(display_name);
    free(hyperlink_target);
    free(symlink_display);

    if (fclose(stream) != 0) {
        free(line);
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }

    *line_out = line;
    return true;
}

static void bx_ls_print_long_entry(
    const struct bx_ls_entry* entry,
    const struct bx_ls_options* options,
    const struct bx_ls_long_widths* widths,
    struct bx_diag_ctx* diag) {
    char* line = NULL;
    if (!bx_ls_format_long_entry_line(entry, options, widths, diag, &line, NULL, NULL)) {
        return;
    }
    (void)fputs(line, stdout);
    bx_ls_put_line_terminator(options);
    free(line);
}

static uintmax_t bx_ls_total_allocated_bytes(const struct bx_ls_entry_list* entries, struct bx_diag_ctx* diag) {
    uintmax_t total_bytes = 0;

    for (size_t i = 0; i < entries->len; i++) {
        struct stat st;
        if (!bx_ls_entry_stat(&entries->items[i], &st, diag, false)) {
            continue;
        }

        total_bytes += bx_ls_allocated_bytes(&st);
    }

    return total_bytes;
}

static void bx_ls_print_entries_long_with_widths(const struct bx_ls_entry_list* entries,
                                                 const struct bx_ls_options* options,
                                                 const struct bx_ls_long_widths* widths,
                                                 bool print_total,
                                                 struct bx_diag_ctx* diag) {
    if (print_total) {
        char total_text[32];
        bx_ls_format_block_count(bx_ls_total_allocated_bytes(entries, diag), options, total_text);
        printf("total %s", total_text);
        bx_ls_put_line_terminator(options);
    }

    for (size_t i = 0; i < entries->len; i++) {
        bx_ls_print_long_entry(&entries->items[i], options, widths, diag);
    }
}

static void bx_ls_print_entries_long(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_total) {
    struct bx_ls_long_widths widths;
    bx_ls_compute_long_widths(entries, options, diag, &widths);
    bx_ls_print_entries_long_with_widths(entries, options, &widths, print_total, diag);
}

static void bx_ls_print_entries_long_dired_with_widths(const struct bx_ls_entry_list* entries,
                                                       const struct bx_ls_options* options,
                                                       const struct bx_ls_long_widths* widths,
                                                       struct bx_diag_ctx* diag,
                                                       bool print_total,
                                                       struct bx_ls_dired_output* output) {
    if (print_total) {
        char total_text[32];
        bx_ls_format_block_count(bx_ls_total_allocated_bytes(entries, diag), options, total_text);
        (void)fprintf(output->stream, "  total %s\n", total_text);
    }

    for (size_t i = 0; i < entries->len; i++) {
        char* line = NULL;
        size_t name_start = 0u;
        size_t name_end = 0u;
        if (!bx_ls_format_long_entry_line(&entries->items[i], options, widths, diag, &line, &name_start, &name_end)) {
            continue;
        }

        size_t base = bx_ls_dired_current_offset(output);
        (void)fputs("  ", output->stream);
        (void)fputs(line, output->stream);
        (void)fputc('\n', output->stream);
        bx_ls_dired_record_name_range(output, base + 2u + name_start, base + 2u + name_end);
        free(line);
    }
}

static void bx_ls_print_entries_long_dired(const struct bx_ls_entry_list* entries,
                                           const struct bx_ls_options* options,
                                           struct bx_diag_ctx* diag,
                                           bool print_total,
                                           struct bx_ls_dired_output* output) {
    struct bx_ls_long_widths widths;
    bx_ls_compute_long_widths(entries, options, diag, &widths);
    bx_ls_print_entries_long_dired_with_widths(entries, options, &widths, diag, print_total, output);
}

static void bx_ls_dired_record_name_range(struct bx_ls_dired_output* output, size_t start, size_t end) {
    bx_ls_dired_output_append_range(&output->name_ranges, &output->name_len, &output->name_cap, start, end);
}

static void bx_ls_dired_record_subdir_range(struct bx_ls_dired_output* output, size_t start, size_t end) {
    bx_ls_dired_output_append_range(&output->subdir_ranges, &output->subdir_len, &output->subdir_cap, start, end);
}

static size_t bx_ls_dired_current_offset(const struct bx_ls_dired_output* output) {
    long pos = ftell(output->stream);
    if (pos < 0) {
        return output->len;
    }
    return (size_t)pos;
}

static void bx_ls_dired_write_directory_header(struct bx_ls_dired_output* output,
                                               const char* dir_path,
                                               bool record_subdir) {
    size_t base = bx_ls_dired_current_offset(output);
    (void)fputs("  ", output->stream);
    (void)fputs(dir_path, output->stream);
    (void)fputs(":\n", output->stream);
    if (record_subdir) {
        bx_ls_dired_record_subdir_range(output, base + 2u, base + 2u + strlen(dir_path));
    }
}

static void bx_ls_dired_emit_output(const struct bx_ls_dired_output* output, const struct bx_ls_options* options) {
    (void)fwrite(output->data, 1u, output->len, stdout);

    (void)fputs("//DIRED//", stdout);
    for (size_t i = 0; i < output->name_len; i++) {
        (void)fprintf(stdout, " %zu %zu", output->name_ranges[i].start, output->name_ranges[i].end);
    }
    (void)fputc('\n', stdout);

    if (output->subdir_len > 0u) {
        (void)fputs("//SUBDIRED//", stdout);
        for (size_t i = 0; i < output->subdir_len; i++) {
            (void)fprintf(stdout, " %zu %zu", output->subdir_ranges[i].start, output->subdir_ranges[i].end);
        }
        (void)fputc('\n', stdout);
    }

    (void)fprintf(stdout, "//DIRED-OPTIONS// --quoting-style=%s\n", bx_ls_quoting_style_name(options));
}

static void bx_ls_print_entries(const struct bx_ls_entry_list* entries, const struct bx_ls_options* options, struct bx_diag_ctx* diag, bool print_total) {
    if (print_total && options->format != BX_LS_FORMAT_LONG) {
        char total_text[32];
        bx_ls_format_block_count(bx_ls_total_allocated_bytes(entries, diag), options, total_text);
        printf("total %s", total_text);
        bx_ls_put_line_terminator(options);
    }

    switch (options->format) {
        case BX_LS_FORMAT_LONG:
            bx_ls_print_entries_long(entries, options, diag, print_total);
            break;
        case BX_LS_FORMAT_COMMAS:
            bx_ls_print_entries_commas(entries, options, diag);
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

static void bx_ls_list_directory(const char* dir_path,
                                 const struct bx_ls_options* options,
                                 struct bx_diag_ctx* diag,
                                 bool print_header,
                                 bool is_command_line_dir,
                                 struct bx_ls_dir_stack* dir_stack,
                                 struct bx_ls_dired_output* dired_output) {
    if (print_header) {
        if (dired_output != NULL) {
            bx_ls_dired_write_directory_header(dired_output, dir_path, true);
        }
        else {
            printf("%s:\n", dir_path);
        }
    }

    struct bx_ls_entry_list entries = {0};
    int error_status = is_command_line_dir ? 2 : 1;
    (void)bx_ls_collect_directory_entries(dir_path, options, &entries, diag, error_status);
    if (dired_output != NULL) {
        bx_ls_print_entries_long_dired(&entries, options, diag, true, dired_output);
    }
    else {
        bx_ls_print_entries(&entries, options, diag, options->format == BX_LS_FORMAT_LONG || options->show_size_blocks);
    }

    struct stat current_dir_st;
    bool pushed_current_dir = false;
    if (options->recursive && dir_stack != NULL && stat(dir_path, &current_dir_st) == 0) {
        bx_ls_dir_stack_push(dir_stack, &current_dir_st);
        pushed_current_dir = true;
    }

    if (options->recursive) {
        for (size_t i = 0; i < entries.len; i++) {
            struct bx_ls_entry* entry = &entries.items[i];
            struct stat recurse_st;
            if (!bx_ls_entry_stat(entry, &recurse_st, diag, false) || !S_ISDIR(recurse_st.st_mode)) {
                continue;
            }
            if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
                continue;
            }

            if (dir_stack != NULL && bx_ls_dir_stack_contains(dir_stack, &recurse_st)) {
                bx_ls_already_listed_dir_error(diag, entry->full_path);
                continue;
            }

            if (dired_output != NULL) {
                (void)fputc('\n', dired_output->stream);
            }
            else {
                (void)fputc('\n', stdout);
            }
            bx_ls_list_directory(entry->full_path, options, diag, true, false, dir_stack, dired_output);
        }
    }

    if (pushed_current_dir) {
        bx_ls_dir_stack_pop(dir_stack);
    }

    bx_ls_entry_list_free(&entries);
}

static void bx_ls_add_operand_as_entry(
    const char* operand,
    const struct stat* st,
    bool follow_for_display,
    struct bx_ls_entry_list* file_entries) {
    struct bx_ls_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = xstrdup(operand);
    entry.full_path = xstrdup(operand);
    entry.st = *st;
    entry.has_stat = true;
    entry.follow_for_display = follow_for_display;
    if (follow_for_display) {
        (void)bx_ls_entry_load_stat(&entry, true);
    }
    bx_ls_entry_list_append(file_entries, &entry);
}

static bool bx_ls_follow_command_line_symlink_dir_by_default(const struct bx_ls_options* options) {
    return options->format != BX_LS_FORMAT_LONG
        && options->indicator_style != BX_LS_INDICATOR_CLASSIFY;
}

static void bx_ls_entry_list_append_copy(struct bx_ls_entry_list* list, const struct bx_ls_entry* src) {
    struct bx_ls_entry entry = *src;
    entry.name = xstrdup(src->name);
    entry.full_path = xstrdup(src->full_path);
    bx_ls_entry_list_append(list, &entry);
}

static void bx_ls_compute_file_section_long_widths(const struct bx_ls_entry_list* file_entries,
                                                   const struct bx_ls_path_list* directory_paths,
                                                   const struct bx_ls_options* options,
                                                   struct bx_diag_ctx* diag,
                                                   struct bx_ls_long_widths* widths) {
    struct bx_ls_entry_list combined = {0};

    for (size_t i = 0; i < file_entries->len; i++) {
        bx_ls_entry_list_append_copy(&combined, &file_entries->items[i]);
    }

    for (size_t i = 0; i < directory_paths->len; i++) {
        const char* path = directory_paths->items[i];
        struct stat st;
        if (lstat(path, &st) != 0) {
            continue;
        }

        bool follow_for_display = options->dereference_all
            || options->dereference_command_line
            || options->dereference_command_line_symlink_to_dir;
        bx_ls_add_operand_as_entry(path, &st, follow_for_display, &combined);
    }

    bx_ls_compute_long_widths(&combined, options, diag, widths);
    bx_ls_entry_list_free(&combined);
}

static void bx_ls_classify_operand(const char* operand, const struct bx_ls_options* options, struct bx_ls_entry_list* file_entries, struct bx_ls_path_list* directory_paths, struct bx_diag_ctx* diag) {
    struct stat lst;
    if (lstat(operand, &lst) != 0) {
        bx_ls_perror_path(diag, operand, 2);
        return;
    }

    struct stat followed;
    bool have_followed = false;
    if (S_ISLNK(lst.st_mode)) {
        have_followed = (stat(operand, &followed) == 0);
    }

    bool target_is_dir = have_followed && S_ISDIR(followed.st_mode);
    bool follow_for_display = false;

    if (S_ISLNK(lst.st_mode) && have_followed) {
        if (options->dereference_all || options->dereference_command_line) {
            follow_for_display = true;
        }
        else if (options->directory_mode && target_is_dir && options->dereference_command_line_symlink_to_dir) {
            follow_for_display = true;
        }
    }

    bool list_target_dir_contents = false;
    if (target_is_dir) {
        list_target_dir_contents = options->dereference_all
            || options->dereference_command_line
            || options->dereference_command_line_symlink_to_dir
            || bx_ls_follow_command_line_symlink_dir_by_default(options);
    }

    if (!options->directory_mode && (S_ISDIR(lst.st_mode) || list_target_dir_contents)) {
        bx_ls_path_list_append(directory_paths, operand);
        return;
    }

    bx_ls_add_operand_as_entry(operand, &lst, follow_for_display, file_entries);
}

static void bx_ls_run(const struct bx_ls_options* options, int argc, char** argv, int first_operand, struct bx_diag_ctx* diag) {
    struct bx_ls_entry_list file_entries = {0};
    struct bx_ls_path_list directory_paths = {0};
    bool use_dired_output = options->dired && options->format == BX_LS_FORMAT_LONG;
    struct bx_ls_dired_output dired_output = {0};
    struct bx_ls_dir_stack dir_stack = {0};

    if (use_dired_output && !bx_ls_dired_output_init(&dired_output, diag)) {
        return;
    }

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
        if (options->format == BX_LS_FORMAT_LONG && directory_paths.len > 0u) {
            struct bx_ls_long_widths widths;
            bx_ls_compute_file_section_long_widths(&file_entries, &directory_paths, options, diag, &widths);
            if (use_dired_output) {
                bx_ls_print_entries_long_dired_with_widths(&file_entries, options, &widths, diag, false, &dired_output);
            }
            else {
                bx_ls_print_entries_long_with_widths(&file_entries, options, &widths, false, diag);
            }
        }
        else if (use_dired_output) {
            bx_ls_print_entries_long_dired(&file_entries, options, diag, false, &dired_output);
        }
        else {
            bx_ls_print_entries(&file_entries, options, diag, false);
        }
        wrote_output = true;
    }

    bool print_directory_headers = options->recursive || file_entries.len > 0 || directory_paths.len > 1;
    for (size_t i = 0; i < directory_paths.len; i++) {
        if (wrote_output) {
            if (use_dired_output) {
                (void)fputc('\n', dired_output.stream);
            }
            else {
                (void)fputc('\n', stdout);
            }
        }
        bx_ls_list_directory(directory_paths.items[i],
                             options,
                             diag,
                             print_directory_headers,
                             true,
                             &dir_stack,
                             use_dired_output ? &dired_output : NULL);
        wrote_output = true;
    }

    if (use_dired_output && bx_ls_dired_output_close(&dired_output, diag)) {
        bx_ls_dired_emit_output(&dired_output, options);
    }

    bx_ls_entry_list_free(&file_entries);
    bx_ls_path_list_free(&directory_paths);
    bx_ls_dir_stack_free(&dir_stack);
    bx_ls_dired_output_free(&dired_output);
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
        int status = diag.exit_status != 0 ? diag.exit_status : 1;
        if (status != 0) {
            bx_cli_print_try_help(diag.progname);
        }
        bx_ls_options_free(&options);
        return status;
    }

    if (options.show_help) {
        bx_ls_print_help(stdout, &options);
        bx_ls_options_free(&options);
        return 0;
    }

    if (options.show_version) {
        bx_cli_print_version(options.progname);
        bx_ls_options_free(&options);
        return 0;
    }

    if (options.dired && options.format == BX_LS_FORMAT_LONG && options.zero_terminated) {
        bx_diag(&diag, "--dired and --zero are incompatible");
        bx_ls_options_free(&options);
        return 2;
    }

    bx_ls_run(&options, argc, argv, first_operand, &diag);

    if (fflush(stdout) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
    }

    int status = diag.exit_status;
    bx_ls_options_free(&options);
    return status;
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
