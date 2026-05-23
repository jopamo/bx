#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "bx/diag.h"
#include "bx/libbx.h"
#include "lib/cli_common.h"
#include "tree_internal.h"

enum bx_tree_option_code {
    BX_TREE_OPT_HELP = 256,
    BX_TREE_OPT_VERSION,
    BX_TREE_OPT_NOREPORT,
    BX_TREE_OPT_NO_LINKS,
    BX_TREE_OPT_INODES,
    BX_TREE_OPT_DEVICE,
    BX_TREE_OPT_DIRSFIRST,
    BX_TREE_OPT_FILELIMIT,
    BX_TREE_OPT_MATCHDIRS,
    BX_TREE_OPT_IGNORE_CASE,
    BX_TREE_OPT_CHARSET,
};

static void bx_tree_print_help(FILE *stream, const char *progname) {
    fprintf(stream, "Usage: %s [-adfghilnopqrstuvxACDFNS] [-L level [-R]] [-H baseHREF]\n", progname);
    fprintf(stream, "       %*s[-T title] [-o filename] [--nolinks] [-P pattern] [-I pattern]\n", (int)strlen(progname) + 8, "");
    fprintf(stream, "       %*s[--inodes] [--device] [--noreport] [--dirsfirst] [--version]\n", (int)strlen(progname) + 8, "");
    fprintf(stream, "       %*s[--help] [--filelimit #] [directory ...]\n", (int)strlen(progname) + 8, "");
    fprintf(stream, "List contents of directories in a tree-like format.\n\n");
    fprintf(stream, "  -a             include hidden files\n");
    fprintf(stream, "  -d             list directories only\n");
    fprintf(stream, "  -f             print full path prefix for each file\n");
    fprintf(stream, "  -i             do not print indentation lines\n");
    fprintf(stream, "  -l             follow symlinked directories\n");
    fprintf(stream, "  -x             stay on the current file system\n");
    fprintf(stream, "  -P pattern     show only names matching pattern\n");
    fprintf(stream, "  -I pattern     hide names matching pattern\n");
    fprintf(stream, "  --matchdirs    apply -P to directory names and include matched directory contents\n");
    fprintf(stream, "  --ignore-case  make -P and -I case-insensitive\n");
    fprintf(stream, "  --prune        omit empty directories from the output\n");
    fprintf(stream, "  --noreport     omit the file and directory count summary\n");
    fprintf(stream, "  -p             print file type and permissions\n");
    fprintf(stream, "  -s             print file size in bytes\n");
    fprintf(stream, "  -h             print file size in human-readable units\n");
    fprintf(stream, "  -u             print the user name or uid\n");
    fprintf(stream, "  -g             print the group name or gid\n");
    fprintf(stream, "  -D             print the modification time\n");
    fprintf(stream, "  --inodes       print inode numbers\n");
    fprintf(stream, "  --device       print device numbers\n");
    fprintf(stream, "  -F             append file type indicators\n");
    fprintf(stream, "  -q             replace control characters in names with '?'\n");
    fprintf(stream, "  -N             print names literally\n");
    fprintf(stream, "  -v             sort by version\n");
    fprintf(stream, "  -r             reverse the selected sort order\n");
    fprintf(stream, "  -t             sort by modification time\n");
    fprintf(stream, "  --dirsfirst    list directories before non-directories\n");
    fprintf(stream, "  -n             turn color off\n");
    fprintf(stream, "  -C             turn color on\n");
    fprintf(stream, "  -A             use ANSI line graphics\n");
    fprintf(stream, "  -S             use IBM437 line graphics\n");
    fprintf(stream, "  -L level       limit recursion depth\n");
    fprintf(stream, "  --filelimit N  do not descend directories with more than N entries\n");
    fprintf(stream, "  -H baseHREF    emit HTML output with links relative to baseHREF\n");
    fprintf(stream, "  -T title       set HTML title and H1 text\n");
    fprintf(stream, "  --charset X    set the HTML and line-drawing character set\n");
    fprintf(stream, "  --nolinks      disable hyperlinks in HTML output\n");
    fprintf(stream, "  -R             write 00Tree.html into each visible directory (HTML only)\n");
    fprintf(stream, "  -o file        write output to file\n");
    fprintf(stream, "  --help         display this help and exit\n");
    fprintf(stream, "  --version      output version information and exit\n");
}

static bool bx_tree_parse_nonnegative(const char *progname,
                                      const char *optname,
                                      const char *text,
                                      int *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || value < 0 || value > 1L << 20) {
        fprintf(stderr, "%s: invalid argument for %s: %s\n", progname, optname,
                text ? text : "(null)");
        return false;
    }
    *out = (int)value;
    return true;
}

static bool bx_tree_parse_charset(const char *progname,
                                  const char *text,
                                  struct bx_tree_options *opts) {
    if (!text || strcmp(text, "UTF-8") == 0 || strcmp(text, "utf-8") == 0 ||
        strcmp(text, "UTF8") == 0 || strcmp(text, "utf8") == 0) {
        opts->charset_mode = BX_TREE_CHARSET_UTF8;
        opts->charset_name = "UTF-8";
        return true;
    }
    if (strcmp(text, "ASCII") == 0 || strcmp(text, "ascii") == 0) {
        opts->charset_mode = BX_TREE_CHARSET_ASCII;
        opts->charset_name = "US-ASCII";
        return true;
    }
    if (strcmp(text, "IBM437") == 0 || strcmp(text, "ibm437") == 0 ||
        strcmp(text, "cp437") == 0) {
        opts->charset_mode = BX_TREE_CHARSET_IBM437;
        opts->charset_name = "IBM437";
        return true;
    }
    if (strcmp(text, "ANSI") == 0 || strcmp(text, "ansi") == 0 ||
        strcmp(text, "VT100") == 0 || strcmp(text, "vt100") == 0) {
        opts->charset_mode = BX_TREE_CHARSET_VT100;
        opts->charset_name = "ANSI";
        return true;
    }

    fprintf(stderr, "%s: invalid argument for --charset: %s\n", progname, text);
    return false;
}

static void bx_tree_options_init(struct bx_tree_options *opts, const char *progname) {
    memset(opts, 0, sizeof(*opts));
    opts->progname = progname;
    opts->name_mode = BX_TREE_NAME_CARET;
    opts->sort_mode = BX_TREE_SORT_NAME;
    opts->charset_mode = BX_TREE_CHARSET_UTF8;
    opts->charset_name = getenv("TREE_CHARSET");
    if (!opts->charset_name || !*opts->charset_name)
        opts->charset_name = "UTF-8";
    opts->max_depth = -1;
    opts->filelimit = -1;
}

static bool bx_tree_parse_options(int argc,
                                  char **argv,
                                  struct bx_tree_options *opts,
                                  int *first_operand) {
    static const struct option long_options[] = {
        {"help", no_argument, NULL, BX_TREE_OPT_HELP},
        {"version", no_argument, NULL, BX_TREE_OPT_VERSION},
        {"noreport", no_argument, NULL, BX_TREE_OPT_NOREPORT},
        {"nolinks", no_argument, NULL, BX_TREE_OPT_NO_LINKS},
        {"inodes", no_argument, NULL, BX_TREE_OPT_INODES},
        {"device", no_argument, NULL, BX_TREE_OPT_DEVICE},
        {"dirsfirst", no_argument, NULL, BX_TREE_OPT_DIRSFIRST},
        {"filelimit", required_argument, NULL, BX_TREE_OPT_FILELIMIT},
        {"matchdirs", no_argument, NULL, BX_TREE_OPT_MATCHDIRS},
        {"ignore-case", no_argument, NULL, BX_TREE_OPT_IGNORE_CASE},
        {"prune", no_argument, NULL, 'Z'},
        {"charset", required_argument, NULL, BX_TREE_OPT_CHARSET},
        {NULL, 0, NULL, 0},
    };

    opterr = 0;
    optind = 1;

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "+:adfghilnpqrstuvxACDFNSL:RH:T:o:P:I:",
                            long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {
        case 'a':
            opts->show_all = true;
            break;
        case 'd':
            opts->dirs_only = true;
            break;
        case 'f':
            opts->full_path = true;
            break;
        case 'g':
            opts->show_group = true;
            break;
        case 'h':
            opts->show_size = true;
            opts->human_size = true;
            break;
        case 'i':
            opts->no_indentation = true;
            break;
        case 'l':
            opts->follow_symlink_dirs = true;
            break;
        case 'n':
            opts->colorize = false;
            break;
        case 'o':
            opts->output_path = optarg;
            break;
        case 'p':
            opts->show_mode = true;
            break;
        case 'q':
            opts->name_mode = BX_TREE_NAME_QUESTION;
            break;
        case 'r':
            opts->reverse_sort = true;
            break;
        case 's':
            opts->show_size = true;
            break;
        case 't':
            opts->sort_mode = BX_TREE_SORT_MTIME;
            break;
        case 'u':
            opts->show_user = true;
            break;
        case 'v':
            opts->sort_mode = BX_TREE_SORT_VERSION;
            break;
        case 'x':
            opts->stay_on_filesystem = true;
            break;
        case 'A':
            opts->charset_mode = BX_TREE_CHARSET_VT100;
            opts->charset_name = "ANSI";
            break;
        case 'C':
            opts->colorize = true;
            break;
        case 'D':
            opts->show_date = true;
            break;
        case 'F':
            opts->classify = true;
            break;
        case 'N':
            opts->name_mode = BX_TREE_NAME_LITERAL;
            break;
        case 'S':
            opts->charset_mode = BX_TREE_CHARSET_IBM437;
            opts->charset_name = "IBM437";
            break;
        case 'L':
            if (!bx_tree_parse_nonnegative(opts->progname, "-L", optarg, &opts->max_depth))
                return false;
            break;
        case 'R':
            opts->recursive_html = true;
            break;
        case 'H':
            opts->html_output = true;
            opts->html_base_href = optarg;
            break;
        case 'T':
            opts->html_title = optarg;
            break;
        case 'P':
            opts->pattern_include = optarg;
            break;
        case 'I':
            opts->pattern_exclude = optarg;
            break;
        case 'Z':
            opts->prune = true;
            break;
        case BX_TREE_OPT_HELP:
            bx_tree_print_help(stdout, opts->progname);
            exit(0);
        case BX_TREE_OPT_VERSION:
            bx_cli_print_version(opts->progname);
            exit(0);
        case BX_TREE_OPT_NOREPORT:
            opts->no_report = true;
            break;
        case BX_TREE_OPT_NO_LINKS:
            opts->html_no_links = true;
            break;
        case BX_TREE_OPT_INODES:
            opts->show_inode = true;
            break;
        case BX_TREE_OPT_DEVICE:
            opts->show_device = true;
            break;
        case BX_TREE_OPT_DIRSFIRST:
            opts->dirs_first = true;
            break;
        case BX_TREE_OPT_FILELIMIT:
            if (!bx_tree_parse_nonnegative(opts->progname, "--filelimit", optarg,
                                           &opts->filelimit))
                return false;
            break;
        case BX_TREE_OPT_MATCHDIRS:
            opts->match_dirs = true;
            break;
        case BX_TREE_OPT_IGNORE_CASE:
            opts->ignore_case = true;
            break;
        case BX_TREE_OPT_CHARSET:
            if (!bx_tree_parse_charset(opts->progname, optarg, opts))
                return false;
            break;
        case ':':
            if (optind > 0 && optind <= argc && strncmp(argv[optind - 1], "--", 2) == 0)
                fprintf(stderr, "%s: option requires an argument -- '%s'\n",
                        opts->progname, argv[optind - 1]);
            else if (optopt != 0)
                fprintf(stderr, "%s: option requires an argument -- '%c'\n",
                        opts->progname, optopt);
            else
                fprintf(stderr, "%s: option requires an argument\n", opts->progname);
            return false;
        case '?':
        default:
            if (optind > 0 && optind <= argc)
                fprintf(stderr, "%s: unrecognized option '%s'\n", opts->progname, argv[optind - 1]);
            else
                fprintf(stderr, "%s: invalid option\n", opts->progname);
            return false;
        }
    }

    *first_operand = optind;
    return true;
}

static FILE *bx_tree_open_output(const struct bx_tree_options *opts,
                                 struct bx_diag_ctx *diag) {
    if (!opts->output_path)
        return stdout;

    FILE *stream = fopen(opts->output_path, "w");
    if (!stream) {
        bx_diag(diag, "%s: %s", opts->output_path, strerror(errno));
        return NULL;
    }
    return stream;
}

int bx_tree_main(int argc, char **argv) {
    const char *progname = bx_cli_progname((argc > 0) ? argv[0] : NULL, "tree");
    struct bx_tree_options opts;
    bx_tree_options_init(&opts, progname);

    int first_operand = 1;
    if (!bx_tree_parse_options(argc, argv, &opts, &first_operand)) {
        bx_cli_print_try_help(progname);
        return 1;
    }

    if (opts.recursive_html && !opts.html_output) {
        fprintf(stderr, "%s: -R requires -H\n", progname);
        return 1;
    }

    int operand_count = argc - first_operand;
    const char **operands = NULL;
    if (operand_count <= 0) {
        static const char *default_operand = ".";
        operands = &default_operand;
        operand_count = 1;
    } else {
        const char **explicit_operands = xmalloc((size_t)operand_count * sizeof(*explicit_operands));
        for (int i = 0; i < operand_count; i++)
            explicit_operands[i] = argv[first_operand + i];
        operands = explicit_operands;
    }

    if (opts.html_output && operand_count > 1) {
        fprintf(stderr, "%s: HTML output accepts at most one directory operand\n", progname);
        return 1;
    }

    struct bx_diag_ctx diag = {.progname = progname};
    struct bx_tree_root *roots = calloc((size_t)operand_count, sizeof(*roots));
    if (!roots) {
        bx_diag(&diag, "memory allocation failed");
        return 1;
    }

    bool had_error = false;
    size_t root_count = 0u;
    for (int i = 0; i < operand_count; i++) {
        bool walk_failed = false;
        if (!bx_tree_build_root(operands[i], &opts, &roots[root_count], &walk_failed)) {
            had_error = true;
            continue;
        }
        bx_tree_apply_visibility(&roots[root_count], &opts);
        bx_tree_sort_visible(&roots[root_count], &opts);
        had_error = had_error || walk_failed;
        root_count++;
    }

    struct bx_tree_meta_widths widths = {0};
    for (size_t i = 0; i < root_count; i++) {
        struct bx_tree_meta_widths one = {0};
        bx_tree_collect_meta_widths(&roots[i], &opts, &one);
        if (one.inode > widths.inode)
            widths.inode = one.inode;
        if (one.device > widths.device)
            widths.device = one.device;
        if (one.user > widths.user)
            widths.user = one.user;
        if (one.group > widths.group)
            widths.group = one.group;
        if (one.size > widths.size)
            widths.size = one.size;
    }

    unsigned long total_directories = 0;
    unsigned long total_files = 0;
    for (size_t i = 0; i < root_count; i++)
        bx_tree_count_visible(&roots[i], &opts, &total_directories, &total_files);

    FILE *stream = bx_tree_open_output(&opts, &diag);
    if (!stream) {
        for (size_t i = 0; i < root_count; i++)
            bx_tree_free_root(&roots[i]);
        free(roots);
        return 1;
    }

    bool write_failed = false;
    for (size_t i = 0; i < root_count && !write_failed; i++) {
        if (i > 0 && !opts.html_output) {
            if (fputc('\n', stream) == EOF)
                write_failed = true;
        }

        bool ok;
        if (opts.html_output) {
            ok = bx_tree_render_html(stream, &roots[i], &opts, &widths, &diag,
                                     -1, opts.html_title, opts.html_base_href);
            if (ok && opts.recursive_html)
                ok = bx_tree_render_recursive_html(&roots[i], &opts, &widths, &diag);
        } else {
            ok = bx_tree_render_plain(stream, &roots[i], &opts, &widths, &diag);
        }
        if (!ok)
            write_failed = true;
    }

    if (!write_failed && !opts.no_report && !opts.html_output) {
        if (fprintf(stream, "\n%lu director%s, %lu file%s\n",
                    total_directories,
                    total_directories == 1 ? "y" : "ies",
                    total_files,
                    total_files == 1 ? "" : "s") < 0) {
            bx_diag(&diag, "write error: %s", strerror(errno));
            write_failed = true;
        }
    }

    if (!write_failed && fflush(stream) == EOF) {
        bx_diag(&diag, "write error: %s", strerror(errno));
        write_failed = true;
    }
    if (stream != stdout && fclose(stream) != 0) {
        bx_diag(&diag, "write error: %s", strerror(errno));
        write_failed = true;
    }

    for (size_t i = 0; i < root_count; i++)
        bx_tree_free_root(&roots[i]);
    free(roots);
    if (argc - first_operand > 0)
        free((void *)operands);

    return (had_error || write_failed || root_count == 0u) ? 1 : 0;
}
