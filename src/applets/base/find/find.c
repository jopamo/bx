#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "applets.h"
#include "find_internal.h"

void find_report_error(const char *progname, const char *path, int errnum) {
    fprintf(stderr, "%s: %s: %s\n", progname, path, strerror(errnum));
}

static void find_print_help(const char *progname) {
    printf("Usage: %s [PATH]... [EXPRESSION]\n", progname);
    puts("Search for files in a directory hierarchy.");
    puts("");
    puts("  -H            follow command-line symlinks");
    puts("  -L            follow all symlinks");
    puts("  -P            never follow symlinks");
    puts("  -depth        process directory contents before the directory");
    puts("  -files0-from FILE  read starting points from a NUL-delimited FILE");
    puts("  -mount        do not descend directories on other filesystems");
    puts("  -maxdepth N   descend at most N levels below the roots");
    puts("  -mindepth N   do not act on levels less than N");
    puts("  -xdev         same as -mount");
    puts("  -name PATTERN match basename against PATTERN");
    puts("  -lname PATTERN match symlink target against PATTERN");
    puts("  -regex PATTERN match whole path against PATTERN");
    puts("  -iregex PATTERN match whole path against PATTERN, case-insensitively");
    puts("  -regextype TYPE  select regex syntax (currently: default, posix-extended)");
    puts("  -type [fdl]   match file type");
    puts("  -xtype [fdl]  match the alternate type across symlink dereference");
    puts("  -inum N       match inode number");
    puts("  -links N      match link count");
    puts("  -uid N        match user id");
    puts("  -gid N        match group id");
    puts("  -user NAME    match user name or numeric uid");
    puts("  -group NAME   match group name or numeric gid");
    puts("  -nouser       match files whose uid has no passwd entry");
    puts("  -nogroup      match files whose gid has no group entry");
    puts("  -perm MODE    match permission bits");
    puts("  -size N[cwbkMG]  match file size");
    puts("  -amin N       match access age in minutes");
    puts("  -atime N      match access age in 24-hour days");
    puts("  -cmin N       match status-change age in minutes");
    puts("  -ctime N      match status-change age in 24-hour days");
    puts("  -mmin N       match modification age in minutes");
    puts("  -mtime N      match modification age in 24-hour days");
    puts("  -used N       match access age measured from last status change");
    puts("  -anewer FILE  match entries accessed more recently than FILE was modified");
    puts("  -cnewer FILE  match entries changed more recently than FILE was modified");
    puts("  -newer FILE   match entries newer than FILE");
    puts("  -true         always true");
    puts("  -false        always false");
    puts("  -print        print path");
    puts("  -print0       print path followed by NUL");
    puts("  -printf FORMAT  write formatted output");
    puts("  -ls           list entry in a GNU find -ls style format");
    puts("  -fprintf FILE FORMAT  write formatted output to FILE");
    puts("  -fls FILE     write -ls style output to FILE");
    puts("  -fprint FILE  write path to FILE");
    puts("  -fprint0 FILE write path followed by NUL to FILE");
    puts("  -delete       delete matched entries");
    puts("  -prune        do not descend into matched directories");
    puts("  -quit         stop after the first deciding result");
    puts("  -exec CMD ... {} ;  run CMD once per matched path");
    puts("  -ok CMD ... {} ;    prompt, then run CMD once per matched path");
    puts("  -exec CMD ... {} +  run CMD with batched matched paths");
    puts("  -execdir CMD ... {} ;  run CMD once per matched path from its parent directory");
    puts("  -okdir CMD ... {} ;  prompt, then run CMD once per matched path from its parent directory");
    puts("  -execdir CMD ... {} +  run CMD with batched matched paths from their parent directory");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

static void find_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

int bx_find_main(int argc, char **argv) {
    const char *progname = argv[0] ? argv[0] : "find";
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        find_print_help(progname);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        find_print_version(progname);
        return 0;
    }

    struct find_opts opts = {
        .max_depth = -1,
        .min_depth = 0,
    };

    int argi = 1;
    while (argi < argc) {
        if (strcmp(argv[argi], "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
            argi++;
        } else if (strcmp(argv[argi], "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
            argi++;
        } else {
            break;
        }
    }

    int expr_index = argi;
    while (expr_index < argc && !find_token_starts_expression(argv[expr_index]))
        expr_index++;

    int explicit_root_count = expr_index - argi;
    char **explicit_roots = argv + argi;

    char **expr_argv = NULL;
    int expr_argc = 0;
    if (!find_collect_expression_argv(progname, argc, argv, expr_index, &opts,
                                      &expr_argv, &expr_argc)) {
        return 1;
    }

    struct find_root_list root_list = {0};
    char **roots = explicit_roots;
    int root_count = explicit_root_count;
    if (opts.files0_from) {
        if (explicit_root_count > 0) {
            fprintf(stderr, "%s: extra operand `%s'\n", progname,
                    explicit_roots[0]);
            fprintf(stderr,
                    "%s: file operands cannot be combined with -files0-from\n",
                    progname);
            free(expr_argv);
            return 1;
        }
        if (!find_load_files0_roots(progname, opts.files0_from, &root_list)) {
            free(expr_argv);
            find_root_list_free(&root_list);
            return 1;
        }
        roots = root_list.v;
        root_count = root_list.count;
    } else if (root_count == 0) {
        static char *default_root[] = {"."};
        roots = default_root;
        root_count = 1;
    }

    struct find_expr *expr = NULL;
    if (!find_prepare_expression(progname, &opts, expr_argv, expr_argc, &expr)) {
        free(expr_argv);
        find_root_list_free(&root_list);
        return 1;
    }

    int status = find_run_search(progname, &opts, expr, roots, root_count);
    find_expr_free(expr);
    free(expr_argv);
    find_root_list_free(&root_list);
    return status;
}
