#ifndef BX_APPLETS_BASE_FIND_INTERNAL_H
#define BX_APPLETS_BASE_FIND_INTERNAL_H

#include <regex.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

#include "find_regex.h"
#include "search/walk.h"

struct find_opts {
    bool depth_first;
    int max_depth;
    int min_depth;
    bool follow_symlinks;
    bool follow_root_symlink;
    bool stay_on_filesystem;
    const char *files0_from;
};

enum find_expr_kind {
    FIND_EXPR_TRUE,
    FIND_EXPR_FALSE,
    FIND_EXPR_NAME,
    FIND_EXPR_PATH,
    FIND_EXPR_REGEX,
    FIND_EXPR_LNAME,
    FIND_EXPR_TYPE,
    FIND_EXPR_XTYPE,
    FIND_EXPR_INUM,
    FIND_EXPR_LINKS,
    FIND_EXPR_UID,
    FIND_EXPR_GID,
    FIND_EXPR_USER,
    FIND_EXPR_GROUP,
    FIND_EXPR_NOUSER,
    FIND_EXPR_NOGROUP,
    FIND_EXPR_PERM,
    FIND_EXPR_SIZE,
    FIND_EXPR_AMIN,
    FIND_EXPR_ATIME,
    FIND_EXPR_CMIN,
    FIND_EXPR_CTIME,
    FIND_EXPR_MMIN,
    FIND_EXPR_MTIME,
    FIND_EXPR_USED,
    FIND_EXPR_ANEWER,
    FIND_EXPR_CNEWER,
    FIND_EXPR_NEWER,
    FIND_EXPR_EMPTY,
    FIND_EXPR_READABLE,
    FIND_EXPR_WRITABLE,
    FIND_EXPR_EXECUTABLE,
    FIND_EXPR_PRINT,
    FIND_EXPR_PRINT0,
    FIND_EXPR_PRINTF,
    FIND_EXPR_LS,
    FIND_EXPR_FPRINTF,
    FIND_EXPR_FLS,
    FIND_EXPR_FPRINT,
    FIND_EXPR_FPRINT0,
    FIND_EXPR_DELETE,
    FIND_EXPR_PRUNE,
    FIND_EXPR_QUIT,
    FIND_EXPR_EXEC,
    FIND_EXPR_OK,
    FIND_EXPR_EXEC_PLUS,
    FIND_EXPR_EXECDIR,
    FIND_EXPR_OKDIR,
    FIND_EXPR_EXECDIR_PLUS,
    FIND_EXPR_NOT,
    FIND_EXPR_AND,
    FIND_EXPR_OR,
    FIND_EXPR_COMMA,
};

struct find_exec_items {
    char **v;
    int count;
    int cap;
};

struct find_root_list {
    char **v;
    int count;
    int cap;
};

struct find_expr {
    enum find_expr_kind kind;
    struct find_expr *left;
    struct find_expr *right;
    const char *text;
    const char *text2;
    char type_filter;
    bool ignore_case;
    long long number;
    int number_cmp;
    mode_t perm_bits;
    int perm_kind;
    unsigned long long size_unit;
    struct timespec ref_time;
    char **exec_argv;
    int exec_argc;
    struct find_exec_items exec_items;
    regex_t regex;
    bool regex_compiled;
};

struct find_parser {
    const char *progname;
    char **argv;
    int argc;
    int pos;
    bool explicit_action;
    struct find_opts *opts;
    enum find_regex_type regex_type;
};

struct find_state {
    const char *progname;
    struct find_opts *opts;
    struct find_expr *expr;
    bool *stop;
    int status;
    struct timespec now;
};

void find_report_error(const char *progname, const char *path, int errnum);

struct find_expr *find_expr_new(enum find_expr_kind kind);
struct find_expr *find_make_binary(enum find_expr_kind kind,
                                   struct find_expr *left,
                                   struct find_expr *right);
bool find_exec_items_append(struct find_exec_items *items, char *text);
void find_exec_items_free(struct find_exec_items *items);
void find_expr_free(struct find_expr *expr);

struct find_expr *find_parse_expr(struct find_parser *parser);
bool find_eval_expr(struct find_expr *expr, struct walk_entry *entry,
                    struct find_state *st);
bool find_token_starts_expression(const char *arg);
bool find_collect_expression_argv(const char *progname, int argc, char **argv,
                                  int start, struct find_opts *opts,
                                  char ***expr_argv_out, int *expr_argc_out);
bool find_prepare_expression(const char *progname, struct find_opts *opts,
                             char **expr_argv, int expr_argc,
                             struct find_expr **expr_out);
int find_run_search(const char *progname, struct find_opts *opts,
                    struct find_expr *expr, char **roots, int root_count);
bool find_load_files0_roots(const char *progname, const char *source,
                            struct find_root_list *roots);
void find_root_list_free(struct find_root_list *roots);

#endif
