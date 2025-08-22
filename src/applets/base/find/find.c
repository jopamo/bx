#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "applets.h"
#include "bx/diag.h"
#include "search/walk.h"

struct find_opts {
    bool depth_first;
    int max_depth;
    int min_depth;
    bool follow_symlinks;
    bool follow_root_symlink;
};

enum find_expr_kind {
    FIND_EXPR_TRUE,
    FIND_EXPR_FALSE,
    FIND_EXPR_NAME,
    FIND_EXPR_PATH,
    FIND_EXPR_LNAME,
    FIND_EXPR_TYPE,
    FIND_EXPR_EMPTY,
    FIND_EXPR_READABLE,
    FIND_EXPR_WRITABLE,
    FIND_EXPR_EXECUTABLE,
    FIND_EXPR_PRINT,
    FIND_EXPR_PRINT0,
    FIND_EXPR_FPRINT,
    FIND_EXPR_FPRINT0,
    FIND_EXPR_DELETE,
    FIND_EXPR_QUIT,
    FIND_EXPR_NOT,
    FIND_EXPR_AND,
    FIND_EXPR_OR,
    FIND_EXPR_COMMA,
};

struct find_expr {
    enum find_expr_kind kind;
    struct find_expr *left;
    struct find_expr *right;
    const char *text;
    char type_filter;
    bool ignore_case;
};

struct find_parser {
    const char *progname;
    char **argv;
    int argc;
    int pos;
    bool explicit_action;
};

struct find_state {
    const char *progname;
    struct find_opts *opts;
    struct find_expr *expr;
    bool *stop;
    int status;
};

static void find_report_error(const char *progname, const char *path, int errnum) {
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
    puts("  -maxdepth N   descend at most N levels below the roots");
    puts("  -mindepth N   do not act on levels less than N");
    puts("  -name PATTERN match basename against PATTERN");
    puts("  -lname PATTERN match symlink target against PATTERN");
    puts("  -type [fdl]   match file type");
    puts("  -true         always true");
    puts("  -false        always false");
    puts("  -print        print path");
    puts("  -print0       print path followed by NUL");
    puts("  -fprint FILE  write path to FILE");
    puts("  -fprint0 FILE write path followed by NUL to FILE");
    puts("  -delete       delete matched entries");
    puts("  -quit         stop after the first deciding result");
    puts("      --help    display this help and exit");
    puts("      --version output version information and exit");
}

static void find_print_version(const char *progname) {
    printf("%s (bx) %s\n", progname, BX_VERSION);
}

static bool parse_int_arg(const char *progname, const char *optname, const char *text, int *out) {
    char *end = NULL;
    long v = strtol(text, &end, 10);
    if (!text || *text == '\0' || (end && *end != '\0') || v < 0 || v > 1<<20) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname, text ? text : "(null)");
        return false;
    }
    *out = (int)v;
    return true;
}

static const char *find_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static bool find_match_pattern(const char *pattern, const char *text, bool ignore_case) {
    return fnmatch(pattern, text, ignore_case ? FNM_CASEFOLD : 0) == 0;
}

static bool find_match_link_target(const struct walk_entry *entry, const char *pattern, bool ignore_case) {
    if (!S_ISLNK(entry->mode))
        return false;

    char buf[PATH_MAX + 1];
    ssize_t len = readlink(entry->path, buf, PATH_MAX);
    if (len < 0)
        return false;
    buf[len] = '\0';
    return find_match_pattern(pattern, buf, ignore_case);
}

static bool find_write_path_file(const char *progname, const char *filename, const char *path, char terminator) {
    FILE *fp = fopen(filename, "ab");
    if (!fp) {
        find_report_error(progname, filename, errno);
        return false;
    }
    size_t path_len = strlen(path);
    bool ok = fwrite(path, 1, path_len, fp) == path_len && fputc(terminator, fp) != EOF;
    if (!ok)
        find_report_error(progname, filename, errno ? errno : EIO);
    fclose(fp);
    return ok;
}

static bool find_matches_type(const struct walk_entry *entry, char type_filter) {
    switch (type_filter) {
    case 'f':
        return S_ISREG(entry->mode);
    case 'd':
        return S_ISDIR(entry->mode);
    case 'l':
        return S_ISLNK(entry->mode);
    case 'p':
        return S_ISFIFO(entry->mode);
    case 's':
        return S_ISSOCK(entry->mode);
    case 'b':
        return S_ISBLK(entry->mode);
    case 'c':
        return S_ISCHR(entry->mode);
    default:
        return false;
    }
}

static bool find_is_empty(const struct walk_entry *entry) {
    if (S_ISREG(entry->mode)) {
        struct stat st;
        if (stat(entry->path, &st) != 0)
            return false;
        return st.st_size == 0;
    }
    if (!S_ISDIR(entry->mode))
        return false;

    DIR *dir = opendir(entry->path);
    if (!dir)
        return false;
    struct dirent *ent;
    bool empty = true;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            empty = false;
            break;
        }
    }
    closedir(dir);
    return empty;
}

static struct find_expr *find_expr_new(enum find_expr_kind kind) {
    struct find_expr *expr = calloc(1, sizeof(*expr));
    if (!expr)
        return NULL;
    expr->kind = kind;
    return expr;
}

static void find_expr_free(struct find_expr *expr) {
    if (!expr)
        return;
    find_expr_free(expr->left);
    find_expr_free(expr->right);
    free(expr);
}

static bool token_starts_expression(const char *arg) {
    if (!arg || arg[0] == '\0')
        return false;
    return arg[0] == '-' || arg[0] == '!' || arg[0] == '(' || arg[0] == ')'|| strcmp(arg, ",") == 0;
}

static bool find_is_and_token(const char *arg) {
    return arg && (strcmp(arg, "-a") == 0 || strcmp(arg, "-and") == 0);
}

static bool find_is_or_token(const char *arg) {
    return arg && (strcmp(arg, "-o") == 0 || strcmp(arg, "-or") == 0);
}

static bool find_is_not_token(const char *arg) {
    return arg && (strcmp(arg, "!") == 0 || strcmp(arg, "-not") == 0);
}

static bool find_is_primary_start(const char *arg) {
    if (!arg)
        return false;
    if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0)
        return false;
    if (find_is_and_token(arg) || find_is_or_token(arg))
        return false;
    return token_starts_expression(arg);
}

static struct find_expr *find_parse_expr(struct find_parser *parser);

static struct find_expr *find_make_binary(enum find_expr_kind kind,
                                          struct find_expr *left,
                                          struct find_expr *right) {
    struct find_expr *expr = find_expr_new(kind);
    if (!expr) {
        find_expr_free(left);
        find_expr_free(right);
        return NULL;
    }
    expr->left = left;
    expr->right = right;
    return expr;
}

static struct find_expr *find_parse_primary(struct find_parser *parser) {
    if (parser->pos >= parser->argc) {
        fprintf(stderr, "%s: expected an expression\n", parser->progname);
        return NULL;
    }

    const char *arg = parser->argv[parser->pos++];
    if (strcmp(arg, "(") == 0) {
        struct find_expr *expr = find_parse_expr(parser);
        if (!expr)
            return NULL;
        if (parser->pos >= parser->argc || strcmp(parser->argv[parser->pos], ")") != 0) {
            fprintf(stderr, "%s: expected ')'\n", parser->progname);
            find_expr_free(expr);
            return NULL;
        }
        parser->pos++;
        return expr;
    }

    struct find_expr *expr = NULL;
    if (strcmp(arg, "-true") == 0) {
        expr = find_expr_new(FIND_EXPR_TRUE);
    } else if (strcmp(arg, "-false") == 0) {
        expr = find_expr_new(FIND_EXPR_FALSE);
    } else if (strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_NAME);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iname") == 0;
        }
    } else if (strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 || strcmp(arg, "-iwholename") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_PATH);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iwholename") == 0;
        }
    } else if (strcmp(arg, "-lname") == 0 || strcmp(arg, "-ilname") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_LNAME);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-ilname") == 0;
        }
    } else if (strcmp(arg, "-type") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `-type'\n", parser->progname);
            return NULL;
        }
        const char *type_arg = parser->argv[parser->pos++];
        if (type_arg[0] == '\0' || type_arg[1] != '\0' ||
            (type_arg[0] != 'f' && type_arg[0] != 'd' && type_arg[0] != 'l' &&
             type_arg[0] != 'p' && type_arg[0] != 's' && type_arg[0] != 'b' &&
             type_arg[0] != 'c')) {
            fprintf(stderr, "%s: unknown argument to -type: %s\n", parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_TYPE);
        if (expr)
            expr->type_filter = type_arg[0];
    } else if (strcmp(arg, "-empty") == 0) {
        expr = find_expr_new(FIND_EXPR_EMPTY);
    } else if (strcmp(arg, "-readable") == 0) {
        expr = find_expr_new(FIND_EXPR_READABLE);
    } else if (strcmp(arg, "-writable") == 0) {
        expr = find_expr_new(FIND_EXPR_WRITABLE);
    } else if (strcmp(arg, "-executable") == 0) {
        expr = find_expr_new(FIND_EXPR_EXECUTABLE);
    } else if (strcmp(arg, "-print") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_PRINT);
    } else if (strcmp(arg, "-print0") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_PRINT0);
    } else if (strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) {
        if (parser->pos >= parser->argc) {
            fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname, arg);
            return NULL;
        }
        parser->explicit_action = true;
        expr = find_expr_new(strcmp(arg, "-fprint") == 0 ? FIND_EXPR_FPRINT : FIND_EXPR_FPRINT0);
        if (expr)
            expr->text = parser->argv[parser->pos++];
    } else if (strcmp(arg, "-delete") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_DELETE);
    } else if (strcmp(arg, "-quit") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_QUIT);
    } else {
        fprintf(stderr, "%s: unknown predicate `%s'\n", parser->progname, arg);
        return NULL;
    }

    if (!expr)
        fprintf(stderr, "%s: out of memory\n", parser->progname);
    return expr;
}

static struct find_expr *find_parse_not(struct find_parser *parser) {
    if (parser->pos < parser->argc && find_is_not_token(parser->argv[parser->pos])) {
        parser->pos++;
        struct find_expr *child = find_parse_not(parser);
        if (!child)
            return NULL;
        struct find_expr *expr = find_expr_new(FIND_EXPR_NOT);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            find_expr_free(child);
            return NULL;
        }
        expr->left = child;
        return expr;
    }
    return find_parse_primary(parser);
}

static struct find_expr *find_parse_and(struct find_parser *parser) {
    struct find_expr *expr = find_parse_not(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc) {
        const char *arg = parser->argv[parser->pos];
        if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0 || find_is_or_token(arg))
            break;
        if (find_is_and_token(arg))
            parser->pos++;
        else if (!find_is_primary_start(arg))
            break;

        struct find_expr *rhs = find_parse_not(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_AND, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static struct find_expr *find_parse_or(struct find_parser *parser) {
    struct find_expr *expr = find_parse_and(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc && find_is_or_token(parser->argv[parser->pos])) {
        parser->pos++;
        struct find_expr *rhs = find_parse_and(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_OR, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static struct find_expr *find_parse_expr(struct find_parser *parser) {
    struct find_expr *expr = find_parse_or(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc && strcmp(parser->argv[parser->pos], ",") == 0) {
        parser->pos++;
        struct find_expr *rhs = find_parse_or(parser);
        if (!rhs) {
            find_expr_free(expr);
            return NULL;
        }
        expr = find_make_binary(FIND_EXPR_COMMA, expr, rhs);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", parser->progname);
            return NULL;
        }
    }

    return expr;
}

static bool find_eval_expr(const struct find_expr *expr, const struct walk_entry *entry,
                           struct find_state *st) {
    if (!expr)
        return true;
    if (st->stop && *st->stop)
        return false;

    switch (expr->kind) {
    case FIND_EXPR_TRUE:
        return true;
    case FIND_EXPR_FALSE:
        return false;
    case FIND_EXPR_NAME:
        return find_match_pattern(expr->text, find_basename(entry->path), expr->ignore_case);
    case FIND_EXPR_PATH:
        return find_match_pattern(expr->text, entry->path, expr->ignore_case);
    case FIND_EXPR_LNAME:
        return find_match_link_target(entry, expr->text, expr->ignore_case);
    case FIND_EXPR_TYPE:
        return find_matches_type(entry, expr->type_filter);
    case FIND_EXPR_EMPTY:
        return find_is_empty(entry);
    case FIND_EXPR_READABLE:
        return access(entry->path, R_OK) == 0;
    case FIND_EXPR_WRITABLE:
        return access(entry->path, W_OK) == 0;
    case FIND_EXPR_EXECUTABLE:
        return access(entry->path, X_OK) == 0;
    case FIND_EXPR_PRINT:
        printf("%s\n", entry->path);
        return true;
    case FIND_EXPR_PRINT0:
        printf("%s%c", entry->path, '\0');
        return true;
    case FIND_EXPR_FPRINT:
        if (!find_write_path_file(st->progname, expr->text, entry->path, '\n')) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_FPRINT0:
        if (!find_write_path_file(st->progname, expr->text, entry->path, '\0')) {
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_DELETE:
        if (strcmp(entry->path, ".") == 0) {
            errno = EBUSY;
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        if ((entry->is_dir ? rmdir(entry->path) : unlink(entry->path)) != 0) {
            find_report_error(st->progname, entry->path, errno);
            st->status = 1;
            if (st->stop)
                *st->stop = true;
            return false;
        }
        return true;
    case FIND_EXPR_QUIT:
        if (st->stop)
            *st->stop = true;
        return true;
    case FIND_EXPR_NOT:
        return !find_eval_expr(expr->left, entry, st);
    case FIND_EXPR_AND: {
        bool lhs = find_eval_expr(expr->left, entry, st);
        if (!lhs || (st->stop && *st->stop))
            return lhs;
        return find_eval_expr(expr->right, entry, st);
    }
    case FIND_EXPR_OR: {
        bool lhs = find_eval_expr(expr->left, entry, st);
        if (lhs || (st->stop && *st->stop))
            return lhs;
        return find_eval_expr(expr->right, entry, st);
    }
    case FIND_EXPR_COMMA:
        (void)find_eval_expr(expr->left, entry, st);
        if (st->stop && *st->stop)
            return false;
        return find_eval_expr(expr->right, entry, st);
    }

    return false;
}

static void find_walk_cb(const struct walk_entry *entry, void *user) {
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
    while (expr_index < argc && !token_starts_expression(argv[expr_index]))
        expr_index++;

    int root_count = expr_index - argi;
    char **roots = argv + argi;
    if (root_count == 0) {
        static char *default_root[] = { "." };
        roots = default_root;
        root_count = 1;
    }

    char **expr_argv = calloc((size_t)(argc - expr_index + 1), sizeof(*expr_argv));
    if (!expr_argv) {
        fprintf(stderr, "%s: out of memory\n", progname);
        return 1;
    }

    int expr_argc = 0;
    for (int i = expr_index; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-L") == 0) {
            opts.follow_symlinks = true;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-H") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = true;
        } else if (strcmp(arg, "-P") == 0) {
            opts.follow_symlinks = false;
            opts.follow_root_symlink = false;
        } else if (strcmp(arg, "-depth") == 0) {
            opts.depth_first = true;
        } else if (strcmp(arg, "-maxdepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-maxdepth", argv[i], &opts.max_depth)) {
                free(expr_argv);
                return 1;
            }
        } else if (strcmp(arg, "-mindepth") == 0) {
            if (++i >= argc || !parse_int_arg(progname, "-mindepth", argv[i], &opts.min_depth)) {
                free(expr_argv);
                return 1;
            }
        } else {
            expr_argv[expr_argc++] = argv[i];
            if ((strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0 ||
                 strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 ||
                 strcmp(arg, "-iwholename") == 0 || strcmp(arg, "-lname") == 0 ||
                 strcmp(arg, "-ilname") == 0 || strcmp(arg, "-type") == 0 ||
                 strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) && i + 1 < argc) {
                expr_argv[expr_argc++] = argv[++i];
            }
        }
    }

    struct find_parser parser = {
        .progname = progname,
        .argv = expr_argv,
        .argc = expr_argc,
    };

    struct find_expr *expr = NULL;
    if (expr_argc > 0) {
        expr = find_parse_expr(&parser);
        if (!expr || parser.pos != parser.argc) {
            find_expr_free(expr);
            free(expr_argv);
            return 1;
        }
    } else {
        expr = find_expr_new(FIND_EXPR_TRUE);
    }

    if (!expr) {
        free(expr_argv);
        return 1;
    }

    if (parser.explicit_action == false) {
        struct find_expr *print_expr = find_expr_new(FIND_EXPR_PRINT);
        expr = find_make_binary(FIND_EXPR_AND, expr, print_expr);
        if (!expr) {
            fprintf(stderr, "%s: out of memory\n", progname);
            free(expr_argv);
            return 1;
        }
    }

    if (expr_argc > 0) {
        for (int i = 0; i < expr_argc; i++) {
            if (strcmp(expr_argv[i], "-delete") == 0) {
                opts.depth_first = true;
                break;
            }
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

    struct walk_opts wopts = {
        .hidden = true,
        .no_ignore = true,
        .follow_symlinks = opts.follow_symlinks,
        .follow_root_symlink = opts.follow_root_symlink,
        .post_order = opts.depth_first,
        .stop = &stop,
        .suppress_eacces = false,
        .os_error_style = false,
        .error_prefix = progname,
        .max_depth = opts.max_depth,
    };

    for (int i = 0; i < root_count && !stop; i++) {
        if (walk_dir(roots[i], &wopts, find_walk_cb, &st) != 0)
            st.status = 1;
    }

    find_expr_free(expr);
    free(expr_argv);
    return st.status;
}
