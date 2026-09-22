#define _GNU_SOURCE
#include "repository.h"
#include "lib/fetch/exit_code.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIRA_GITLAB_API_ROOT "https://gitlab.com/api/v4"

static void gitlab_help(void) {
    fputs("Usage: mira gitlab raw PROJECT REF PATH [OPTION]...\n"
          "Fetch one raw repository file to staged stdout over verified HTTPS.\n"
          "PROJECT may be a numeric ID or namespace/project; components are encoded once.\n"
          "Options: --api-root URL --ca-certificate FILE --no-proxy\n"
          "         --bearer-token TOKEN --bearer-token-file FILE\n"
          "         --max-requests N --max-retry-time SECONDS\n"
          "Authentication defaults to GITLAB_TOKEN on the canonical API root.\n"
          "Custom API roots require explicit credentials.\n", stdout);
}

int bx_mira_gitlab_main(int argc, char** argv) {
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        gitlab_help();
        return 0;
    }
    if (argc < 2 || strcmp(argv[1], "raw") != 0) {
        fputs("mira: gitlab requires raw PROJECT REF PATH\n", stderr);
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    MiraRepositoryOptions options = {0};
    const char* operands[3] = {0};
    int count = 0;
    bool parse_options = true;
    for (int i = 2; i < argc; i++) {
        if (parse_options && strcmp(argv[i], "--") == 0) {
            parse_options = false;
            continue;
        }
        if (parse_options && (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)) {
            gitlab_help();
            return 0;
        }
        int parsed = parse_options ? bx_mira_repository_option(&options, argc, argv, &i) : 0;
        if (parsed < 0 || (parsed == 0 && parse_options && argv[i][0] == '-') ||
            (parsed == 0 && (count == 3 || !argv[i][0]))) {
            fputs("mira: invalid gitlab raw argument\n", stderr);
            return BX_FETCH_EXIT_PARSE_OR_CONFIG;
        }
        if (parsed == 0)
            operands[count++] = argv[i];
    }
    if (count != 3 || strcmp(operands[0], ".") == 0 || strcmp(operands[0], "..") == 0 ||
        strcmp(operands[2], ".") == 0 || strcmp(operands[2], "..") == 0) {
        fputs("mira: gitlab raw requires PROJECT REF PATH\n", stderr);
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    struct bx_fetch_config* config = bx_mira_repository_config(&options, MIRA_GITLAB_API_ROOT, "GITLAB_TOKEN", false);
    if (!config) {
        fputs("mira: could not prepare GitLab request\n", stderr);
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    char* project = bx_fetch_url_encode_component(operands[0], 4096);
    char* ref = bx_fetch_url_encode_component(operands[1], 4096);
    char* path = bx_fetch_url_encode_component(operands[2], 4096);
    char* url = NULL;
    const char* root = options.api_root ? options.api_root : MIRA_GITLAB_API_ROOT;
    if (project && ref && path &&
        asprintf(&url, "/projects/%s/repository/files/%s/raw?ref=%s", project, path, ref) < 0)
        url = NULL;
    if (url) {
        char* endpoint = url;
        url = bx_fetch_url_join_https_root(root, endpoint);
        free(endpoint);
    }
    int result = url ? bx_mira_repository_fetch(config, NULL, url, "-") : BX_FETCH_EXIT_PARSE_OR_CONFIG;
    free(project); free(ref); free(path); free(url);
    bx_fetch_config_free(config);
    return result;
}
