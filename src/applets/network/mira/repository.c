#define _GNU_SOURCE
#include "repository.h"
#include "lib/fetch/credential_file.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/http_header.h"
#include "lib/size_parse.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

int bx_mira_repository_option(MiraRepositoryOptions* options, int argc, char** argv, int* index) {
    const char* argument = argv[*index];
    if (strcmp(argument, "--no-proxy") == 0) {
        options->no_proxy = true;
        return 1;
    }
    struct { const char* name; const char** destination; } strings[] = {
        {"--api-root", &options->api_root}, {"--ca-certificate", &options->ca_certificate},
        {"--bearer-token", &options->token}, {"--bearer-token-file", &options->token_file},
    };
    const char* names[] = {"--max-requests", "--max-retry-time"};
    for (size_t i = 0; i < 6; i++) {
        const char* name = i < 4 ? strings[i].name : names[i - 4];
        size_t length = strlen(name);
        if (strncmp(argument, name, length) != 0 ||
            (argument[length] && argument[length] != '='))
            continue;
        const char* value = argument[length] == '=' ? argument + length + 1 :
            *index + 1 < argc ? argv[++*index] : NULL;
        if (!value || !*value)
            return -1;
        if (i < 4) {
            if (*strings[i].destination)
                return -1;
            *strings[i].destination = value;
        } else {
            uintmax_t number;
            if (!bx_size_parse_uint(value, &number) || !number || number > INT_MAX)
                return -1;
            if (i == 4)
                options->max_requests = (int)number;
            else
                options->max_retry_time = (int)number;
        }
        return 1;
    }
    return 0;
}

struct bx_fetch_config* bx_mira_repository_config(const MiraRepositoryOptions* options,
                                                  const char* default_root, const char* token_environment,
                                                  bool json) {
    const char* root = options->api_root ? options->api_root : default_root;
    if (!bx_fetch_url_is_https_root(root) || (options->token && options->token_file))
        return NULL;
    struct bx_fetch_config* config = bx_mira_config_new();
    if (!config)
        return NULL;
    config->logging.verbosity = BX_FETCH_VERBOSITY_QUIET;
    config->logging.suppress_session_banner = true;
    config->download.show_progress = false;
    config->download.metadata_sidecars = false;
    config->download.no_proxy = options->no_proxy;
    config->download.expected_representation = json ? BX_FETCH_EXPECT_JSON : BX_FETCH_EXPECT_ANY;
    config->download.max_response_bytes = json ? MIRA_REPOSITORY_JSON_LIMIT : 0;
    config->https.require_verified_https = true;
    config->http.provider_rate_limits = true;
    config->http.paranoid = true;
    config->http.max_redirect = 10;
    if (options->max_requests)
        config->download.max_requests = options->max_requests;
    if (options->max_retry_time)
        config->download.max_retry_time = options->max_retry_time;
    free(config->http.redirect_method);
    config->http.redirect_method = strdup("strict");
    if (options->ca_certificate)
        config->https.ca_certificate = strdup(options->ca_certificate);
    const char* token = options->token;
    /* An endpoint override does not authorize sending a canonical provider's
     * environment token there. Explicit credentials remain available. */
    if (!token && !options->token_file && strcmp(root, default_root) == 0)
        token = getenv(token_environment);
    if (token && *token) {
        if (!bx_fetch_http_bearer_token_is_valid(token))
            goto fail;
        config->http.bearer_token = strdup(token);
        if (!config->http.bearer_token)
            goto fail;
    } else if (options->token_file && bx_fetch_bearer_token_load_file(options->token_file, &config->http.bearer_token) != 0) {
        goto fail;
    }
    if (!config->http.redirect_method || (options->ca_certificate && !config->https.ca_certificate))
        goto fail;
    return config;
fail:
    bx_fetch_config_free(config);
    return NULL;
}

int bx_mira_repository_fetch(struct bx_fetch_config* config, BxFetchBudget* budget,
                            const char* url, const char* output) {
    char* operand = strdup(url);
    char* path = strdup(output);
    if (!operand || !path || bx_fetch_config_copy_urls(config, 1, &operand) != 0) {
        free(operand);
        free(path);
        return BX_FETCH_EXIT_FILE_IO;
    }
    free(operand);
    free(config->download.output_document);
    config->download.output_document = path;
    return bx_mira_run_config_with_budget(config, budget);
}
