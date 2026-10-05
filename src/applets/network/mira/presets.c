#define _GNU_SOURCE
#include "mira.h"
#include "mira-profile-config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bx_fetch_config* bx_mira_config_new(void) {
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;
    static const char chrome_agent[] =
        "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/" BX_MIRA_CHROME_MAJOR ".0.0.0 Safari/537.36";
    static const char firefox_agent[] = "Mozilla/5.0 (X11; Linux x86_64; rv:" BX_MIRA_FIREFOX_MAJOR ".0) Gecko/20100101 Firefox/" BX_MIRA_FIREFOX_MAJOR ".0";
    const char* version = bx_fetch_net_curl_version();
    char* user_agent = NULL;
    if (!version || asprintf(&user_agent, "curl/%s", version) < 0)
        goto fail;
    config->http.user_agent = user_agent;
    config->http.profile_agents[BX_FETCH_PROFILE_CURL] = strdup(user_agent);
    char* mira_agent = NULL;
    if (!config->http.profile_agents[BX_FETCH_PROFILE_CURL] || asprintf(&mira_agent, "mira/%s", BX_VERSION) < 0)
        goto fail;
    config->http.profile_agents[BX_FETCH_PROFILE_MIRA] = mira_agent;
    if (BX_MIRA_CHROME_MAJOR[0]) {
        config->http.profile_agents[BX_FETCH_PROFILE_CHROME] = strdup(chrome_agent);
        if (!config->http.profile_agents[BX_FETCH_PROFILE_CHROME])
            goto fail;
    }
    if (BX_MIRA_FIREFOX_MAJOR[0]) {
        config->http.profile_agents[BX_FETCH_PROFILE_FIREFOX] = strdup(firefox_agent);
        if (!config->http.profile_agents[BX_FETCH_PROFILE_FIREFOX])
            goto fail;
    }
    config->http.client_profile = BX_FETCH_PROFILE_AUTO;
    config->download.max_retry_time = 120;
    config->download.max_requests = 64;
    config->download.spider_get_fallback = true;
    config->download.retry_transient_http = true;
    config->download.stdout_spool_limit = UINT64_C(64) * 1024 * 1024;
    return config;
fail:
    bx_fetch_config_free(config);
    return NULL;
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
