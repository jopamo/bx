#include <errno.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "find_internal.h"
#include "find_parse_helpers.h"
#include "lib/id_parse.h"
#include "lib/mode_parse.h"
#include "search/metadata.h"

static bool find_parse_main_int_arg(const char *progname, const char *optname,
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

static bool find_parse_user_id(const char *progname, const char *text,
                               long long *value) {
    uid_t uid = 0;
    if (bx_id_lookup_user(text, &uid)) {
        *value = (long long)uid;
        return true;
    }

    fprintf(stderr, "%s: invalid user name or UID argument to -user: %s\n",
            progname, text);
    return false;
}

static bool find_parse_group_id(const char *progname, const char *text,
                                long long *value) {
    gid_t gid = 0;
    if (bx_id_lookup_group(text, &gid)) {
        *value = (long long)gid;
        return true;
    }

    fprintf(stderr, "%s: invalid group name or GID argument to -group: %s\n",
            progname, text);
    return false;
}

static bool find_parse_perm(const char *progname, const char *text,
                            mode_t *bits, int *kind) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname,
                text ? text : "(null)");
        return false;
    }

    *kind = 0;
    if (*text == '-') {
        *kind = 1;
        text++;
    } else if (*text == '/') {
        *kind = 2;
        text++;
    }

    if (*text == '\0') {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text);
        return false;
    }

    if (bx_mode_parse_numeric(text, 07777u, bits)) {
        if (*kind == 2 && *bits == 0) {
            fprintf(stderr,
                    "%s: warning: you have specified a mode pattern /000 (which is equivalent to /000). "
                    "The meaning of -perm /000 has now been changed to be consistent with -perm -000; "
                    "that is, while it used to match no files, it now matches all files.\n",
                    progname);
        }
        return true;
    }

    struct bx_mode_parse_params params = {
        .initial_mode = 0u,
        .result_mask = 07777u,
        .max_numeric_mode = 07777u,
        .umask_value = 0u,
        .sticky_bit = 01000u,
        .x_policy = BX_MODE_X_DISABLED,
        .is_directory = false,
        .apply_umask_when_who_omitted = false,
        .allow_setuid = true,
        .allow_setgid = true,
        .allow_sticky = true,
    };
    if (!bx_mode_parse_symbolic(text, &params, bits)) {
        fprintf(stderr, "%s: invalid argument to -perm: %s\n", progname, text);
        return false;
    }
    if (*kind == 2 && *bits == 0) {
        fprintf(stderr,
                "%s: warning: you have specified a mode pattern /000 (which is equivalent to /000). "
                "The meaning of -perm /000 has now been changed to be consistent with -perm -000; "
                "that is, while it used to match no files, it now matches all files.\n",
                progname);
    }
    return true;
}

static bool find_parse_size_arg(const char *progname, const char *text,
                                long long *value, int *cmp,
                                unsigned long long *unit) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to -size: %s\n", progname,
                text ? text : "(null)");
        return false;
    }

    *cmp = 0;
    if (*text == '+') {
        *cmp = 1;
        text++;
    } else if (*text == '-') {
        *cmp = -1;
        text++;
    }

    char *end = NULL;
    errno = 0;
    long long v = strtoll(text, &end, 10);
    if (*text == '\0' || !end || errno != 0 || v < 0) {
        fprintf(stderr, "%s: invalid argument to -size: %s\n", progname,
                text ? text : "(null)");
        return false;
    }

    unsigned long long u = 512;
    if (*end != '\0') {
        if (end[1] != '\0') {
            fprintf(stderr, "%s: invalid argument to -size: %s\n", progname,
                    text);
            return false;
        }
        switch (*end) {
        case 'b':
            u = 512;
            break;
        case 'c':
            u = 1;
            break;
        case 'w':
            u = 2;
            break;
        case 'k':
            u = 1024;
            break;
        case 'M':
            u = 1024ULL * 1024ULL;
            break;
        case 'G':
            u = 1024ULL * 1024ULL * 1024ULL;
            break;
        default:
            fprintf(stderr, "%s: invalid argument to -size: %s\n", progname,
                    text);
            return false;
        }
    }

    *value = v;
    *unit = u;
    return true;
}

static bool find_parse_newer_ref(const char *progname, const char *path,
                                 bool follow_root_symlink,
                                 struct timespec *out) {
    struct stat st;
    int rc = follow_root_symlink ? stat(path, &st) : lstat(path, &st);
    if (rc != 0) {
        find_report_error(progname, path, errno);
        return false;
    }
    *out = st.st_mtim;
    return true;
}

bool find_token_starts_expression(const char *arg) {
    if (!arg || arg[0] == '\0')
        return false;
    return arg[0] == '-' || arg[0] == '!' || arg[0] == '(' || arg[0] == ')' ||
           strcmp(arg, ",") == 0;
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
    return find_token_starts_expression(arg);
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

void find_root_list_free(struct find_root_list *roots) {
    if (!roots)
        return;
    for (int i = 0; i < roots->count; i++)
        free(roots->v[i]);
    free(roots->v);
    roots->v = NULL;
    roots->count = 0;
    roots->cap = 0;
}

bool find_load_files0_roots(const char *progname, const char *source,
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
        strcmp(arg, "-newercm") == 0 ||
        strcmp(arg, "-printf") == 0 || strcmp(arg, "-fls") == 0 ||
        strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0)
        return 1;
    return 0;
}

bool find_collect_expression_argv(const char *progname, int argc, char **argv,
                                  int start, struct find_opts *opts,
                                  char ***expr_argv_out, int *expr_argc_out) {
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
            if (++i >= argc || !find_parse_main_int_arg(progname, "-maxdepth",
                                                        argv[i],
                                                        &opts->max_depth)) {
                free(expr_argv);
                return false;
            }
            continue;
        }
        if (strcmp(arg, "-mindepth") == 0) {
            if (++i >= argc || !find_parse_main_int_arg(progname, "-mindepth",
                                                        argv[i],
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

static struct find_expr *find_parse_or(struct find_parser *parser);

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
        if (parser->pos >= parser->argc ||
            strcmp(parser->argv[parser->pos], ")") != 0) {
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
        expr = find_parse_text_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-regex") == 0 || strcmp(arg, "-iregex") == 0) {
        if (!find_parse_require_arguments(parser, arg, 1)) {
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_REGEX);
        if (expr) {
            expr->text = parser->argv[parser->pos++];
            expr->ignore_case = strcmp(arg, "-iregex") == 0;
            if (!find_compile_regex(parser->progname, arg, parser->regex_type,
                                    expr->text, expr->ignore_case,
                                    &expr->regex)) {
                find_expr_free(expr);
                return NULL;
            }
            expr->regex_compiled = true;
        }
    } else if (strcmp(arg, "-regextype") == 0) {
        if (!find_parse_require_arguments(parser, "-regextype", 1)) {
            return NULL;
        }
        if (!find_parse_regextype(parser->progname, parser->argv[parser->pos],
                                  &parser->regex_type)) {
            return NULL;
        }
        parser->pos++;
        expr = find_expr_new(FIND_EXPR_TRUE);
    } else if (strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 ||
               strcmp(arg, "-iwholename") == 0) {
        expr = find_parse_text_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-lname") == 0 || strcmp(arg, "-ilname") == 0) {
        expr = find_parse_text_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-type") == 0) {
        if (!find_parse_require_arguments(parser, "-type", 1)) {
            return NULL;
        }
        const char *type_arg = parser->argv[parser->pos++];
        if (type_arg[0] == '\0' || type_arg[1] != '\0' ||
            !bx_walk_type_filter_is_valid(type_arg[0], false)) {
            fprintf(stderr, "%s: unknown argument to -type: %s\n",
                    parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_TYPE);
        if (expr)
            expr->type_filter = type_arg[0];
    } else if (strcmp(arg, "-xtype") == 0) {
        if (!find_parse_require_arguments(parser, "-xtype", 1)) {
            return NULL;
        }
        const char *type_arg = parser->argv[parser->pos++];
        if (type_arg[0] == '\0' || type_arg[1] != '\0' ||
            !bx_walk_type_filter_is_valid(type_arg[0], false)) {
            fprintf(stderr, "%s: unknown argument to -xtype: %s\n",
                    parser->progname, type_arg);
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_XTYPE);
        if (expr)
            expr->type_filter = type_arg[0];
    } else if (strcmp(arg, "-inum") == 0) {
        expr = find_parse_numeric_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-links") == 0) {
        expr = find_parse_numeric_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-uid") == 0 || strcmp(arg, "-gid") == 0) {
        expr = find_parse_numeric_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-user") == 0 || strcmp(arg, "-group") == 0) {
        if (!find_parse_require_arguments(parser, arg, 1)) {
            return NULL;
        }
        expr = find_expr_new(strcmp(arg, "-user") == 0 ? FIND_EXPR_USER
                                                         : FIND_EXPR_GROUP);
        bool ok = false;
        if (expr && strcmp(arg, "-user") == 0)
            ok = find_parse_user_id(parser->progname, parser->argv[parser->pos],
                                    &expr->number);
        else if (expr)
            ok = find_parse_group_id(parser->progname, parser->argv[parser->pos],
                                     &expr->number);
        if (expr && !ok) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-nouser") == 0) {
        expr = find_expr_new(FIND_EXPR_NOUSER);
    } else if (strcmp(arg, "-nogroup") == 0) {
        expr = find_expr_new(FIND_EXPR_NOGROUP);
    } else if (strcmp(arg, "-perm") == 0) {
        if (!find_parse_require_arguments(parser, "-perm", 1)) {
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_PERM);
        if (expr && !find_parse_perm(parser->progname, parser->argv[parser->pos],
                                     &expr->perm_bits, &expr->perm_kind)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-size") == 0) {
        if (!find_parse_require_arguments(parser, "-size", 1)) {
            return NULL;
        }
        expr = find_expr_new(FIND_EXPR_SIZE);
        if (expr && !find_parse_size_arg(parser->progname, parser->argv[parser->pos],
                                         &expr->number, &expr->number_cmp,
                                         &expr->size_unit)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
    } else if (strcmp(arg, "-amin") == 0 || strcmp(arg, "-atime") == 0 ||
               strcmp(arg, "-cmin") == 0 || strcmp(arg, "-ctime") == 0 ||
               strcmp(arg, "-mmin") == 0 || strcmp(arg, "-mtime") == 0 ||
               strcmp(arg, "-used") == 0) {
        expr = find_parse_numeric_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-anewer") == 0 || strcmp(arg, "-cnewer") == 0 ||
               strcmp(arg, "-newer") == 0 || strcmp(arg, "-newercm") == 0) {
        if (!find_parse_require_arguments(parser, arg, 1)) {
            return NULL;
        }
        enum find_expr_kind kind = FIND_EXPR_NEWER;
        if (strcmp(arg, "-anewer") == 0)
            kind = FIND_EXPR_ANEWER;
        else if (strcmp(arg, "-cnewer") == 0)
            kind = FIND_EXPR_CNEWER;
        else if (strcmp(arg, "-newercm") == 0)
            kind = FIND_EXPR_NEWERCM;
        expr = find_expr_new(kind);
        if (expr &&
            !find_parse_newer_ref(parser->progname, parser->argv[parser->pos],
                                  parser->opts && parser->opts->follow_root_symlink,
                                  &expr->ref_time)) {
            find_expr_free(expr);
            return NULL;
        }
        if (expr)
            parser->pos++;
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
    } else if (strcmp(arg, "-printf") == 0) {
        parser->explicit_action = true;
        expr = find_parse_output_action(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-ls") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_LS);
    } else if (strcmp(arg, "-fprintf") == 0) {
        parser->explicit_action = true;
        expr = find_parse_output_action(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-fls") == 0) {
        parser->explicit_action = true;
        expr = find_parse_output_action(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-fprint") == 0 || strcmp(arg, "-fprint0") == 0) {
        parser->explicit_action = true;
        expr = find_parse_output_action(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-delete") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_DELETE);
    } else if (strcmp(arg, "-prune") == 0) {
        expr = find_expr_new(FIND_EXPR_PRUNE);
    } else if (strcmp(arg, "-quit") == 0) {
        parser->explicit_action = true;
        expr = find_expr_new(FIND_EXPR_QUIT);
    } else if (strcmp(arg, "-exec") == 0 || strcmp(arg, "-execdir") == 0) {
        parser->explicit_action = true;
        expr = find_parse_command_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else if (strcmp(arg, "-ok") == 0 || strcmp(arg, "-okdir") == 0) {
        parser->explicit_action = true;
        expr = find_parse_command_predicate(parser, arg);
        if (!expr)
            return NULL;
    } else {
        fprintf(stderr, "%s: unknown predicate `%s'\n", parser->progname, arg);
        return NULL;
    }

    if (!expr)
        fprintf(stderr, "%s: out of memory\n", parser->progname);
    return expr;
}

static struct find_expr *find_parse_not(struct find_parser *parser) {
    if (parser->pos < parser->argc &&
        find_is_not_token(parser->argv[parser->pos])) {
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
        if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0 ||
            find_is_or_token(arg))
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

    while (parser->pos < parser->argc &&
           find_is_or_token(parser->argv[parser->pos])) {
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

struct find_expr *find_parse_expr(struct find_parser *parser) {
    struct find_expr *expr = find_parse_or(parser);
    if (!expr)
        return NULL;

    while (parser->pos < parser->argc &&
           strcmp(parser->argv[parser->pos], ",") == 0) {
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
