#define _GNU_SOURCE
#include "github.h"
#include "github_internal.h"
#include "repository_json.h"
#include "lib/fetch/credential_file.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/resource_limits.h"
#include "lib/jq/filter.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MIRA_GITHUB_API_VERSION "2022-11-28"
#define MIRA_GITHUB_QUERY_MAX_BYTES ((size_t)4096)

static void github_parse_error(const char* message) {
    fprintf(stderr, "mira: github: %s\n", message);
}

void bx_mira_github_print_help(void) {
    fputs(
        "Usage:\n"
        "  mira github api PATH [KEY=VALUE]... [--jq PROGRAM]\n"
        "  mira github tree OWNER/REPO REF [--recursive]\n"
        "  mira github search-issues OWNER/REPO QUERY [--jq PROGRAM]\n"
        "\n"
        "Read-only GitHub REST API access through Mira's fetch engine.\n"
        "Options:\n"
        "  --bearer-token-file=FILE  read a protected GitHub token file\n"
        "  --bearer-token=TOKEN      use a token from the argument list\n"
        "  --jq=PROGRAM              filter parsed JSON with embedded jq\n"
        "  --json                    select compact JSON for search-issues\n"
        "  --no-proxy                disable proxy use\n"
        "  --api-root=URL            verified HTTPS API root (custom roots need explicit tokens)\n"
        "  --ca-certificate=FILE     trust this certificate authority\n"
        "  --max-requests=N          invocation-wide request limit (default 64)\n"
        "  --max-retry-time=SECONDS  invocation-wide elapsed budget (default 120)\n"
        "  --recursive               include descendants of a GitHub tree\n"
        "  -h, --help                display this help\n"
        "\n"
        "Authentication defaults to the GITHUB_TOKEN environment variable. "
        "An explicit token file or token argument takes precedence.\n",
        stdout);
}

static const char* option_value(const char* argument,
                                const char* name,
                                int* index,
                                int argc,
                                char** argv) {
    size_t name_length = strlen(name);
    if (strncmp(argument, name, name_length) != 0)
        return NULL;
    if (argument[name_length] == '=')
        return argument + name_length + 1u;
    if (argument[name_length] != '\0')
        return NULL;
    if (*index + 1 >= argc)
        return NULL;
    (*index)++;
    return argv[*index];
}

static int parse_github_arguments(int argc,
                                  char** argv,
                                  MiraGithubArguments* arguments) {
    if (!arguments || argc < 2 || !argv)
        return -1;
    arguments->operands = calloc((size_t)argc, sizeof(*arguments->operands));
    if (!arguments->operands)
        return -1;

    bool options = true;
    for (int index = 2; index < argc; index++) {
        const char* argument = argv[index];
        if (options && strcmp(argument, "--") == 0) {
            options = false;
            continue;
        }
        if (options && (strcmp(argument, "-h") == 0 ||
                        strcmp(argument, "--help") == 0)) {
            bx_mira_github_print_help();
            return 1;
        }
        if (options && strcmp(argument, "--json") == 0) {
            arguments->json = true;
            continue;
        }
        if (options && strcmp(argument, "--no-proxy") == 0) {
            arguments->no_proxy = true;
            continue;
        }
        if (options && strncmp(argument, "--jq", 4) == 0) {
            const char* value =
                option_value(argument, "--jq", &index, argc, argv);
            if (!value || value[0] == '\0' || arguments->jq_program) {
                github_parse_error("invalid or duplicate --jq option");
                return -1;
            }
            arguments->jq_program = value;
            continue;
        }
        if (options && strncmp(argument, "--bearer-token-file", 19) == 0) {
            const char* value = option_value(
                argument, "--bearer-token-file", &index, argc, argv);
            if (!value || value[0] == '\0' || arguments->bearer_token_file) {
                github_parse_error(
                    "invalid or duplicate --bearer-token-file option");
                return -1;
            }
            arguments->bearer_token_file = value;
            continue;
        }
        if (options && strncmp(argument, "--bearer-token", 14) == 0) {
            const char* value = option_value(
                argument, "--bearer-token", &index, argc, argv);
            if (!value || value[0] == '\0' || arguments->bearer_token) {
                github_parse_error(
                    "invalid or duplicate --bearer-token option");
                return -1;
            }
            arguments->bearer_token = value;
            continue;
        }
        if (options && strcmp(argument, "--recursive") == 0) {
            arguments->recursive = true;
            continue;
        }
        if (options) {
            int parsed = bx_mira_repository_option(&arguments->repository, argc, argv, &index);
            if (parsed < 0) {
                github_parse_error("invalid repository option");
                return -1;
            }
            if (parsed > 0)
                continue;
        }
        if (options && argument[0] == '-') {
            github_parse_error("unsupported GitHub option");
            return -1;
        }
        arguments->operands[arguments->operand_count++] = argument;
    }

    if (arguments->bearer_token && arguments->bearer_token_file) {
        github_parse_error(
            "--bearer-token and --bearer-token-file are mutually exclusive");
        return -1;
    }
    return 0;
}

bool bx_mira_github_repo_is_valid(const char* repository) {
    if (!repository || repository[0] == '\0')
        return false;
    size_t length = strnlen(repository, 257u);
    if (length == 0 || length > 256u)
        return false;

    const char* slash = strchr(repository, '/');
    if (!slash || slash == repository || slash[1] == '\0' ||
        strchr(slash + 1, '/'))
        return false;
    size_t owner_length = (size_t)(slash - repository);
    if ((owner_length == 1 && repository[0] == '.') ||
        (owner_length == 2 && repository[0] == '.' && repository[1] == '.') ||
        strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0)
        return false;

    for (const char* cursor = repository; *cursor; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        if (cursor == slash)
            continue;
        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.')
            return false;
    }
    return true;
}

static bool github_api_path_is_valid(const char* path) {
    if (!path || path[0] != '/' || path[1] == '\0' ||
        strnlen(path, BX_FETCH_URL_MAX_BYTES + 1u) > BX_FETCH_URL_MAX_BYTES)
        return false;

    const char* segment = path + 1;
    for (const char* cursor = path + 1;; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        if (ch == '\0' || ch == '/') {
            size_t length = (size_t)(cursor - segment);
            if (length == 0 ||
                (length == 1 && segment[0] == '.') ||
                (length == 2 && segment[0] == '.' && segment[1] == '.'))
                return false;
            if (ch == '\0')
                return true;
            segment = cursor + 1;
            continue;
        }
        if (ch < 0x21u || ch > 0x7eu || ch == '?' || ch == '#' || ch == '\\')
            return false;
        if (ch == '%') {
            if (!isxdigit((unsigned char)cursor[1]) ||
                !isxdigit((unsigned char)cursor[2]))
                return false;
            cursor += 2;
        }
    }
}

static char* percent_encode_query_component(const char* input) {
    return bx_fetch_url_encode_component(input, MIRA_GITHUB_QUERY_MAX_BYTES);
}

static char* github_api_url(const MiraGithubArguments* arguments, const char* path,
                            const char* const* query,
                            int query_count) {
    if (!github_api_path_is_valid(path) || query_count < 0)
        return NULL;

    const char* root = arguments->repository.api_root ? arguments->repository.api_root : MIRA_GITHUB_API_ROOT;
    char* url = bx_fetch_url_join_https_root(root, path);
    if (!url)
        return NULL;
    size_t length = strlen(url);

    for (int index = 0; index < query_count; index++) {
        const char* separator = strchr(query[index], '=');
        if (!separator || separator == query[index]) {
            free(url);
            errno = EINVAL;
            return NULL;
        }
        char* name = strndup(query[index], (size_t)(separator - query[index]));
        char* encoded_name = name ? percent_encode_query_component(name) : NULL;
        char* encoded_value = percent_encode_query_component(separator + 1);
        free(name);
        if (!encoded_name || !encoded_value) {
            free(encoded_name);
            free(encoded_value);
            free(url);
            return NULL;
        }

        size_t name_length = strlen(encoded_name);
        size_t value_length = strlen(encoded_value);
        size_t addition = 1u + name_length + 1u + value_length;
        if (length > BX_FETCH_URL_MAX_BYTES ||
            addition > BX_FETCH_URL_MAX_BYTES - length) {
            free(encoded_name);
            free(encoded_value);
            free(url);
            errno = EFBIG;
            return NULL;
        }
        char* grown = realloc(url, length + addition + 1u);
        if (!grown) {
            free(encoded_name);
            free(encoded_value);
            free(url);
            return NULL;
        }
        url = grown;
        url[length++] = index == 0 ? '?' : '&';
        memcpy(url + length, encoded_name, name_length);
        length += name_length;
        url[length++] = '=';
        memcpy(url + length, encoded_value, value_length);
        length += value_length;
        url[length] = '\0';
        free(encoded_name);
        free(encoded_value);
    }
    return url;
}

struct bx_fetch_config* bx_mira_github_config(
    const MiraGithubArguments* arguments,
    const char* url,
    const char* output_path) {
    MiraRepositoryOptions options = arguments->repository;
    options.token = arguments->bearer_token;
    options.token_file = arguments->bearer_token_file;
    options.no_proxy = arguments->no_proxy;
    struct bx_fetch_config* config = bx_mira_repository_config(&options, MIRA_GITHUB_API_ROOT, "GITHUB_TOKEN", true);
    if (!config)
        return NULL;

    free(config->download.output_document);
    config->download.output_document = strdup(output_path);
    char* url_operand = strdup(url);
    if (!config->download.output_document ||
        !config->http.redirect_method || !url_operand ||
        bx_fetch_config_copy_urls(config, 1, &url_operand) != 0 ||
        bx_fetch_config_add_http_header(
            config, "Accept: application/vnd.github+json") !=
            BX_FETCH_HTTP_HEADER_OK ||
        bx_fetch_config_add_http_header(
            config, "X-GitHub-Api-Version: " MIRA_GITHUB_API_VERSION) !=
            BX_FETCH_HTTP_HEADER_OK) {
        free(url_operand);
        bx_fetch_config_free(config);
        return NULL;
    }
    free(url_operand);

    return config;
}

static int github_fetch(const MiraGithubArguments* arguments,
                        const char* url,
                        const char* output_path) {
    struct bx_fetch_config* config =
        bx_mira_github_config(arguments, url, output_path);
    if (!config) {
        github_parse_error("could not prepare GitHub request");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    int result = bx_mira_repository_json(config, NULL, url, NULL);
    bx_fetch_config_free(config);
    return result;
}

static int fetch_and_filter(const MiraGithubArguments* arguments, const char* url, const char* program) {
    struct bx_fetch_config* config = bx_mira_github_config(arguments, url, "-");
    if (!config)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    jv value = jv_invalid();
    int result = bx_mira_repository_json(config, NULL, url, &value);
    bx_fetch_config_free(config);
    if (result == 0 && jv_is_valid(value))
        return bx_mira_repository_print_json(value, program);
    jv_free(value);
    return result;
}

static int github_api(const MiraGithubArguments* arguments) {
    if (arguments->operand_count < 1) {
        github_parse_error("github api requires PATH");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    if (arguments->json) {
        github_parse_error("--json is only valid with search-issues");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    char* url = github_api_url(
        arguments, arguments->operands[0],
        arguments->operands + 1,
        arguments->operand_count - 1);
    if (!url) {
        github_parse_error("invalid GitHub API path or query argument");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    int result = arguments->jq_program
                     ? fetch_and_filter(arguments, url, arguments->jq_program)
                     : github_fetch(arguments, url, "-");
    free(url);
    return result;
}

static int github_search_issues(const MiraGithubArguments* arguments) {
    if (arguments->operand_count != 2 ||
        !bx_mira_github_repo_is_valid(arguments->operands[0])) {
        github_parse_error(
            "search-issues requires OWNER/REPO and one QUERY argument");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    size_t query_length =
        strnlen(arguments->operands[1], MIRA_GITHUB_QUERY_MAX_BYTES + 1u);
    if (query_length == 0 || query_length > MIRA_GITHUB_QUERY_MAX_BYTES) {
        github_parse_error("invalid search query");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    char* qualified = NULL;
    if (asprintf(&qualified,
                 "repo:%s %s",
                 arguments->operands[0],
                 arguments->operands[1]) == -1) {
        qualified = NULL;
    }
    if (!qualified)
        return BX_FETCH_EXIT_FILE_IO;
    char* query = NULL;
    if (asprintf(&query, "q=%s", qualified) == -1)
        query = NULL;
    free(qualified);
    if (!query)
        return BX_FETCH_EXIT_FILE_IO;

    const char* query_arguments[] = {query};
    char* url = github_api_url(arguments, "/search/issues", query_arguments, 1);
    free(query);
    if (!url)
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;

    const char* program = arguments->jq_program
                              ? arguments->jq_program
                              : "{items: [.items[] | "
                                "{number, title, state, url: .html_url}]}";
    int result = fetch_and_filter(arguments, url, program);
    free(url);
    return result;
}

int bx_mira_github_main(int argc, char** argv) {
    if (argc < 2 || !argv) {
        github_parse_error("missing GitHub command");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        bx_mira_github_print_help();
        return BX_FETCH_EXIT_SUCCESS;
    }

    MiraGithubArguments arguments = {0};
    int parse_result = parse_github_arguments(argc, argv, &arguments);
    if (parse_result != 0) {
        free(arguments.operands);
        return parse_result > 0 ? BX_FETCH_EXIT_SUCCESS
                                : BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }

    int result;
    if (arguments.recursive && strcmp(argv[1], "tree") != 0) {
        github_parse_error("--recursive is only valid with tree");
        free(arguments.operands);
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    if (strcmp(argv[1], "tree") == 0)
        result = bx_mira_github_tree(&arguments);
    else if (strcmp(argv[1], "api") == 0)
        result = github_api(&arguments);
    else if (strcmp(argv[1], "search-issues") == 0)
        result = github_search_issues(&arguments);
    else {
        github_parse_error("unknown GitHub command");
        result = BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    free(arguments.operands);
    return result;
}
