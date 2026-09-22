#include "mira.h"
#include <stdlib.h>
#include <string.h>

struct bx_fetch_config* bx_mira_config_new(void) {
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;
    config->download.max_retry_time = 120;
    config->download.max_requests = 64;
    config->download.spider_get_fallback = true;
    config->download.retry_transient_http = true;
    config->download.stdout_spool_limit = UINT64_C(64) * 1024 * 1024;
    return config;
}

int bx_mira_apply_read_preset(struct bx_fetch_config* config) {
    char* output = strdup("-");
    if (!output)
        return -1;
    free(config->download.output_document);
    config->download.output_document = output;
    config->download.text_document = true;
    config->download.show_progress = false;
    config->download.max_retry_time = 30;
    config->logging.verbosity = BX_FETCH_VERBOSITY_QUIET;
    config->https.require_verified_https = true;
    return 0;
}
