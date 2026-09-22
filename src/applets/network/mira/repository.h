#ifndef BX_MIRA_REPOSITORY_H
#define BX_MIRA_REPOSITORY_H

#include "mira.h"

#define MIRA_REPOSITORY_JSON_LIMIT ((size_t)16 * 1024 * 1024)

typedef struct {
    const char* api_root;
    const char* ca_certificate;
    const char* token;
    const char* token_file;
    int max_requests;
    int max_retry_time;
    bool no_proxy;
} MiraRepositoryOptions;

/* Returns 1 for a consumed option, 0 for an unknown option, -1 for invalid. */
int bx_mira_repository_option(MiraRepositoryOptions* options, int argc, char** argv, int* index);
struct bx_fetch_config* bx_mira_repository_config(const MiraRepositoryOptions* options,
                                                  const char* default_root, const char* token_environment,
                                                  bool json);
int bx_mira_repository_fetch(struct bx_fetch_config* config, BxFetchBudget* budget,
                            const char* url, const char* output);

#endif
