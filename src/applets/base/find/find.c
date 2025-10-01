#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "applets.h"
#include "find_exec.h"
#include "find_internal.h"
#include "search/walk.h"

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

static bool parse_int_arg(const char *progname, const char *optname,
                          const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 ||
        v > 1 << 20) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname,
                text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static bool find_root_list_append_copy(struct find_root_list *roots,
                                       const char *text, size_t len) {
    if (roots->count >= roots->cap) {
        int new_cap = roots->cap == 0 ? 8 : roots->cap * 2;
        char **tmp = realloc(roots->v, (size_t)new_cap * sizeof(*roots->v));
        if (!tmp)
            return false;
        roots->v = tmp;
        roots->cap = new_cap;
    }

    char *copy = strndup(text, len);
    if (!copy)
        return false;
    roots->v[roots->count++] = copy;
    return true;
}

static void find_root_list_free(struct find_root_list *roots) {
    if (!roots)
        return;
    for (int i = 0; i < roots->count; i++)
        free(roots->v[i]);
    free(roots->v);
    roots->v = NULL;
    roots->count = 0;
    roots->cap = 0;
}

static bool find_load_files0_roots(const char *progname, const char *source,
                                   struct find_root_list *roots) {
    FILE *fp = NULL;
    if (strcmp(source, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(source, "rb");
        if (!fp) {
            find_report_error(progname, source, errno);
            return false;
        }
    }

    char *item = NULL;
    size_t cap = 0;
    ssize_t len = 0;
    bool ok = true;
    while ((len = getdelim(&item, &cap, '\0', fp)) != -1) {
        size_t item_len = (size_t)len;
        if (item_len > 0 && item[item_len - 1] == '\0')
            item_len--;
        if (item_len == 0)
            continue;
        if (!find_root_list_append_copy(roots, item, item_len)) {
            fprintf(stderr, "%s: out of memory\n", progname);
            ok = false;
            break;
        }
    }

    if (ok && ferror(fp)) {
        find_report_error(progname, source, errno ? errno : EIO);
        ok = false;
    }

    free(item);
    if (fp != stdin)
        fclose(fp);
    return ok;
}

static bool find_token_starts_expression(const char *arg) {
    if (!arg || arg[0] == '\0')
        return false;
    return arg[0] == '-' || arg[0] == '!' || arg[0] == '(' || arg[0] == ')' ||
           strcmp(arg, ",") == 0;
}

static bool find_is_exec_like(const char *arg) {
    return strcmp(arg, "-exec") == 0 || strcmp(arg, "-execdir") == 0;
}

static bool find_is_ok_like(const char *arg) {
    return strcmp(arg, "-ok") == 0 || strcmp(arg, "-okdir") == 0;
}

static int find_expr_fixed_arg_count(const char *arg) {
    if (strcmp(arg, "-fprintf") == 0)
        return 2;
    if (strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0 ||
        strcmp(arg, "-regex") == 0 || strcmp(arg, "-iregex") == 0 ||
        strcmp(arg, "-regextype") == 0 || strcmp(arg, "-path") == 0 ||
        strcmp(arg, "-wholename") == 0 || strcmp(arg, "-iwholename") == 0 ||
        strcmp(arg, "-lname") == 0 || strcmp(arg, "-ilname") == 0 ||
        strcmp(arg, "-type") == 0 || strcmp(arg, "-xtype") == 0 ||
        strcmp(arg, "-inum") == 0 || strcmp(arg, "-links") == 0 ||
        strcmp(arg, "-uid") == 0 || strcmp(arg, "-gid") == 0 ||
        strcmp(arg, "-user") == 0 || strcmp(arg, "-group") == 0 ||
        strcmp(arg, "-perm") == 0 || strcmp(arg, "-size") == 0 ||
        strcmp(arg, "-amin") == 0 || strcmp(arg, "-atime") == 0 ||
        strcmp(arg, "-cmin") == 0 || strcmp(arg, "-ctime") == 0 ||
        strcmp(arg, "-mmin") == 0 || strcmp(arg, "-mtime") == 0 ||
        strcmp(arg, "-used") == 0 || strcmp(arg, "-anewer") == 0 ||
        strcmp(arg, "-cnewer") == 0 || strcmp(arg, "-newer") == 0 ||
        strcmp(arg, "-printf") == 0 || strcmp(arg, "-fls") == 0 ||
        strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0)
        return 1;
    return 0;
}

static bool find_collect_expression_argv(const char *progname, int argc,
                                         char **argv, int start,
                                         struct find_opts *opts,
                                         char ***expr_argv_out,
                                         int *expr_argc_out) {
    char **expr_argv = calloc((size_t)(argc - start + 1), sizeof(*expr_argv));
    if (!expr_argv) {
        fprintf(stderr, "%s: out of memory\n", progname);
        return false;
    }

    int expr_argc = 0;
    for (int i = start; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-L") == 0) {
            opts->follow_symlinks = true;
            opts->follow_root_symlink = true;
            continue;
        }
        if (strcmp(arg, "-H") == 0) {
            opts->follow_symlinks = false;
            opts->follow_root_symlink = true;
            continue;
        }
        if (strcmp(arg, "-P") == 0) {
            opts->follow_symlinks = false;
            opts->follow_root_symlink = false;
            continue;
        }
        if (strcmp(arg, "-depth") == 0) {
            opts->depth_first = true;
            continue;
        }
        if (strcmp(arg, "-maxdepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-maxdepth", argv[i],
                                              &opts->max_depth)) {
                free(expr_argv);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "-mindepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-mindepth", argv[i],
                                              &opts->min_depth)) {
                free(expr_argv);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "-files0-from") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "%s: missing argument to `-files0-from'\n",
                        progname);
                free(expr_argv);
                return false;
            }
            opts->files0_from = argv[i];
            continue;
        }
        if (strcmp(arg, "-mount") == 0 || strcmp(arg, "-xdev") == 0) {
            opts->stay_on_filesystem = true;
            continue;
        }

        expr_argv[expr_argc++] = argv[i];

        if (find_is_exec_like(arg)) {
            while (i + 1 < argc) {
                expr_argv[expr_argc++] = argv[++i];
                if (strcmp(argv[i], ";") == 0 || strcmp(argv[i], "+") == 0)
                    break;
            }
            continue;
        }
        if (find_is_ok_like(arg)) {
            while (i + 1 < argc) {
                expr_argv[expr_argc++] = argv[++i];
                if (strcmp(argv[i], ";") == 0)
                    break;
            }
            continue;
        }

        int fixed_args = find_expr_fixed_arg_count(arg);
        for (int n = 0; n < fixed_args && i + 1 < argc; n++)
            expr_argv[expr_argc++] = argv[++i];
    }

    *expr_argv_out = expr_argv;
    *expr_argc_out = expr_argc;
    return true;
}

static void find_walk_cb(struct walk_entry *entry, void *user) {
    struct find_state *st = user;
    struct find_opts *opts = st->opts;

    if (st->stop && *st->stop)
        return;
    if (entry->depth < opts->min_depth)
        return;
    if (opts->max_depth >= 0 && entry->depth > opts->max_depth)
        return;

    (void)find_eval_expr(st->expr, entry, st);
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

    struct find_parser parser = {
        .progname = progname,
        .argv = expr_argv,
        .argc = expr_argc,
        .opts = &opts,
        .regex_type = FIND_REGEX_TYPE_DEFAULT,
    };

    struct find_expr *expr = NULL;
    if (expr_argc > 0) {
        expr = find_parse_expr(&parser);
        if (!expr || parser.pos != parser.argc) {
            find_expr_free(expr);
            free(expr_argv);
            find_root_list_free(&root_list);
            return 1;
        }
    } else {
        expr = find_expr_new(FIND_EXPR_TRUE);
    }

    if (!expr) {
        free(expr_argv);
        find_root_list_free(&root_list);
        return 1;
    }

    if (!parser.explicit_action) {
        struct find_expr *print_expr = find_expr_new(FIND_EXPR_PRINT);
        expr = find_make_binary(FIND_EXPR_AND, expr, print_expr);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(expr_argv);
            find_root_list_free(&root_list);
            return 1;
        }
    }

    for (int i = 0; i < expr_argc; i++) {
        if (strcmp(expr_argv[i], "-delete") == 0) {
            opts.depth_first = true;
            break;
        }
    }

    bool stop = false;
    struct find_state st = {
        .progname = progname,
        .opts = &opts,
        .expr = expr,
        .stop = &stop,
        .status = 0,
    };
    if (clock_gettime(CLOCK_REALTIME, &st.now) != 0) {
        st.now.tv_sec = time(NULL);
        st.now.tv_nsec = 0;
    }

    struct walk_opts wopts = {
        .hidden = true,
        .no_ignore = true,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = opts.follow_root_symlink,
        .post_order = opts.depth_first,
        .stay_on_filesystem = opts.stay_on_filesystem,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
        .cycle_mode = opts.follow_symlinks ? WALK_CYCLE_DIR_REPEAT
                                           : WALK_CYCLE_NONE,
        .cycle_report = opts.follow_symlinks ? WALK_CYCLE_ERROR
                                             : WALK_CYCLE_IGNORE,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    if (find_interrupt_return_code() != 0 && st.status == 0)
        st.status = find_interrupt_return_code();

    if (find_interrupt_return_code() == 0) {
        int pending_rc = find_run_pending_exec_exprs(progname, expr);
        if (pending_rc > 1)
            st.status = pending_rc;
        else if (pending_rc != 0)
            st.status = 1;
    }

    find_expr_free(expr);
    free(expr_argv);
    find_root_list_free(&root_list);
    return st.status;
}
