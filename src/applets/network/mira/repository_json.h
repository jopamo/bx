#ifndef BX_MIRA_REPOSITORY_JSON_H
#define BX_MIRA_REPOSITORY_JSON_H
#include "repository.h"
#include "lib/fetch/json_document.h"
/* Fetches and parses a complete, bounded JSON response. Caller owns *value.
 * If value is NULL, publishes the validated original bytes instead. */
int bx_mira_repository_json(struct bx_fetch_config* config, BxFetchBudget* budget, const char* url, jv* value);
/* Consumes value; filtered output remains private until the filter succeeds. */
int bx_mira_repository_print_json(jv value, const char* program);

#endif
