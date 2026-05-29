#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "find_parse_helpers.h"

bool find_parse_require_arguments(struct find_parser *parser,
                                  const char *optname, int count) {
    if (parser->pos + count > parser->argc) {
        fprintf(stderr, "%s: missing argument to `%s'\n", parser->progname,
                optname);
        return false;
    }
    return true;
}

static bool find_parse_numeric_test(const char *progname, const char *optname,
                                    const char *text, long long *value,
                                    int *cmp) {
    if (!text || *text == '\0') {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname,
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
    intmax_t v = strtoimax(text, &end, 10);
    if (*text == '\0' || !end || *end != '\0' || errno != 0 || v < 0 ||
        v > (intmax_t)LLONG_MAX) {
        fprintf(stderr, "%s: invalid argument to %s: %s\n", progname, optname,
                text ? text : "(null)");
        return false;
    }
    *value = (long long)v;
    return true;
}

struct find_expr *find_parse_text_predicate(struct find_parser *parser,
                                            const char *arg) {
    enum find_expr_kind kind = FIND_EXPR_NAME;
    bool ignore_case = false;

    if (strcmp(arg, "-name") == 0 || strcmp(arg, "-iname") == 0) {
        kind = FIND_EXPR_NAME;
        ignore_case = strcmp(arg, "-iname") == 0;
    } else if (strcmp(arg, "-path") == 0 || strcmp(arg, "-wholename") == 0 ||
               strcmp(arg, "-iwholename") == 0) {
        kind = FIND_EXPR_PATH;
        ignore_case = strcmp(arg, "-iwholename") == 0;
    } else {
        kind = FIND_EXPR_LNAME;
        ignore_case = strcmp(arg, "-ilname") == 0;
    }

    if (!find_parse_require_arguments(parser, arg, 1))
        return NULL;

    struct find_expr *expr = find_expr_new(kind);
    if (!expr) {
        fprintf(stderr, "%s: out of memory\n", parser->progname);
        return NULL;
    }

    expr->text = parser->argv[parser->pos++];
    expr->ignore_case = ignore_case;
    return expr;
}

static enum find_expr_kind find_numeric_predicate_kind(const char *arg) {
    if (strcmp(arg, "-inum") == 0)
        return FIND_EXPR_INUM;
    if (strcmp(arg, "-links") == 0)
        return FIND_EXPR_LINKS;
    if (strcmp(arg, "-uid") == 0)
        return FIND_EXPR_UID;
    if (strcmp(arg, "-gid") == 0)
        return FIND_EXPR_GID;
    if (strcmp(arg, "-atime") == 0)
        return FIND_EXPR_ATIME;
    if (strcmp(arg, "-cmin") == 0)
        return FIND_EXPR_CMIN;
    if (strcmp(arg, "-ctime") == 0)
        return FIND_EXPR_CTIME;
    if (strcmp(arg, "-mmin") == 0)
        return FIND_EXPR_MMIN;
    if (strcmp(arg, "-mtime") == 0)
        return FIND_EXPR_MTIME;
    if (strcmp(arg, "-used") == 0)
        return FIND_EXPR_USED;
    return FIND_EXPR_AMIN;
}

struct find_expr *find_parse_numeric_predicate(struct find_parser *parser,
                                               const char *arg) {
    if (!find_parse_require_arguments(parser, arg, 1))
        return NULL;

    struct find_expr *expr = find_expr_new(find_numeric_predicate_kind(arg));
    if (!expr) {
        fprintf(stderr, "%s: out of memory\n", parser->progname);
        return NULL;
    }

    if (!find_parse_numeric_test(parser->progname, arg,
                                 parser->argv[parser->pos], &expr->number,
                                 &expr->number_cmp)) {
        find_expr_free(expr);
        return NULL;
    }

    parser->pos++;
    return expr;
}

struct find_expr *find_parse_output_action(struct find_parser *parser,
                                           const char *arg) {
    int arg_count = 1;
    enum find_expr_kind kind = FIND_EXPR_PRINTF;

    if (strcmp(arg, "-printf") == 0) {
        kind = FIND_EXPR_PRINTF;
    } else if (strcmp(arg, "-fprintf") == 0) {
        kind = FIND_EXPR_FPRINTF;
        arg_count = 2;
    } else if (strcmp(arg, "-fls") == 0) {
        kind = FIND_EXPR_FLS;
    } else if (strcmp(arg, "-fprint") == 0) {
        kind = FIND_EXPR_FPRINT;
    } else {
        kind = FIND_EXPR_FPRINT0;
    }

    if (!find_parse_require_arguments(parser, arg, arg_count))
        return NULL;

    struct find_expr *expr = find_expr_new(kind);
    if (!expr) {
        fprintf(stderr, "%s: out of memory\n", parser->progname);
        return NULL;
    }

    expr->text = parser->argv[parser->pos++];
    if (kind == FIND_EXPR_FPRINTF)
        expr->text2 = parser->argv[parser->pos++];
    return expr;
}

static bool find_parse_command_argv(struct find_parser *parser, const char *arg,
                                    bool allow_plus, int *command_end_out,
                                    bool *per_item_out) {
    int command_end = -1;
    bool per_item = false;
    bool saw_placeholder = false;

    for (int i = parser->pos; i < parser->argc; i++) {
        if (strcmp(parser->argv[i], ";") == 0) {
            command_end = i;
            per_item = true;
            break;
        }
        if (allow_plus && strcmp(parser->argv[i], "+") == 0) {
            command_end = i;
            break;
        }
        if (strcmp(parser->argv[i], "{}") == 0)
            saw_placeholder = true;
    }

    if (command_end < 0) {
        if (allow_plus) {
            fprintf(stderr, "%s: missing terminating `;' or `+' for `%s'\n",
                    parser->progname, arg);
        } else {
            fprintf(stderr, "%s: missing terminating `;' for `%s'\n",
                    parser->progname, arg);
        }
        return false;
    }
    if (!saw_placeholder) {
        fprintf(stderr, "%s: missing '{}' in `%s'\n", parser->progname, arg);
        return false;
    }

    *command_end_out = command_end;
    *per_item_out = per_item;
    return true;
}

static enum find_expr_kind find_command_predicate_kind(const char *arg,
                                                       bool per_item) {
    if (strcmp(arg, "-exec") == 0)
        return per_item ? FIND_EXPR_EXEC : FIND_EXPR_EXEC_PLUS;
    if (strcmp(arg, "-execdir") == 0)
        return per_item ? FIND_EXPR_EXECDIR : FIND_EXPR_EXECDIR_PLUS;
    if (strcmp(arg, "-ok") == 0)
        return FIND_EXPR_OK;
    return FIND_EXPR_OKDIR;
}

static bool find_parse_command_vector(struct find_parser *parser,
                                      struct find_expr *expr,
                                      int command_start, int command_end) {
    expr->exec_argc = command_end - command_start;
    expr->exec_argv =
        calloc((size_t)expr->exec_argc + 1, sizeof(*expr->exec_argv));
    if (!expr->exec_argv) {
        fprintf(stderr, "%s: out of memory\n", parser->progname);
        return false;
    }

    for (int i = 0; i < expr->exec_argc; i++)
        expr->exec_argv[i] = parser->argv[command_start + i];
    expr->exec_argv[expr->exec_argc] = NULL;
    return true;
}

struct find_expr *find_parse_command_predicate(struct find_parser *parser,
                                               const char *arg) {
    if (!find_parse_require_arguments(parser, arg, 1))
        return NULL;

    int command_start = parser->pos;
    int command_end = -1;
    bool per_item = false;
    bool allow_plus = strcmp(arg, "-exec") == 0 || strcmp(arg, "-execdir") == 0;

    if (!find_parse_command_argv(parser, arg, allow_plus, &command_end,
                                 &per_item)) {
        return NULL;
    }

    struct find_expr *expr =
        find_expr_new(find_command_predicate_kind(arg, per_item));
    if (!expr) {
        fprintf(stderr, "%s: out of memory\n", parser->progname);
        return NULL;
    }

    if (!find_parse_command_vector(parser, expr, command_start, command_end)) {
        find_expr_free(expr);
        return NULL;
    }

    parser->pos = command_end + 1;
    return expr;
}
