#ifndef BX_APPLETS_BASE_FIND_EXEC_H
#define BX_APPLETS_BASE_FIND_EXEC_H

#include <stdbool.h>

#include "find_internal.h"

bool find_prompt_ok(const char *cmdname, const char *path);
bool find_run_exec_one(struct find_state *st, struct find_expr *expr,
                       const char *path, const char *cwd);
bool find_execdir_split_path(const char *path, char **dir_out, char **arg_out);
int find_run_pending_exec_exprs(const char *progname, struct find_expr *expr);
int find_interrupt_return_code(void);

#endif
