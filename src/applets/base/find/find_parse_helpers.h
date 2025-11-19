#ifndef BX_APPLETS_BASE_FIND_PARSE_HELPERS_H
#define BX_APPLETS_BASE_FIND_PARSE_HELPERS_H

#include "find_internal.h"

bool find_parse_require_arguments(struct find_parser *parser,
                                  const char *optname, int count);
struct find_expr *find_parse_text_predicate(struct find_parser *parser,
                                            const char *arg);
struct find_expr *find_parse_numeric_predicate(struct find_parser *parser,
                                               const char *arg);
struct find_expr *find_parse_output_action(struct find_parser *parser,
                                           const char *arg);
struct find_expr *find_parse_command_predicate(struct find_parser *parser,
                                               const char *arg);

#endif
