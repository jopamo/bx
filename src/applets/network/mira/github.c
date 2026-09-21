#define _GNU_SOURCE
#include "github.h"
#include "mira.h"
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

#define MIRA_GITHUB_API_ROOT "https://api.github.com"
#define MIRA_GITHUB_API_VERSION "2022-11-28"
#define MIRA_GITHUB_JSON_MAX_BYTES ((size_t)16 * 1024u * 1024u)
#define MIRA_GITHUB_QUERY_MAX_BYTES ((size_t)4096)

typedef struct {
    const char* jq_program;
    const char* bearer_token;
    const char* bearer_token_file;
    const char** operands;
    int operand_count;
    bool json;
    bool no_proxy;
} MiraGithubArguments;

typedef struct {
    bool downstream_closed;
} MiraGithubJqOutput;

static void github_parse_error(const char* message) {
    fprintf(stderr, "mira: github: %s\n", message);
}

void bx_mira_github_print_help(void) {
    fputs(
        "Usage:\n"
        "  mira github api PATH [KEY=VALUE]... [--jq PROGRAM]\n"
        "  mira github search-issues OWNER/REPO QUERY [--jq PROGRAM]\n"
        "\n"
        "Read-only GitHub REST API access through Mira's fetch engine.\n"
        "Options:\n"
        "  --bearer-token-file=FILE  read a protected GitHub token file\n"
        "  --bearer-token=TOKEN      use a token from the argument list\n"
        "  --jq=PROGRAM              filter parsed JSON with embedded jq\n"
        "  --json                    select compact JSON for search-issues\n"
        "  --no-proxy                disable proxy use\n"
        "  -h, --help                display this help\n",
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

static bool github_repo_is_valid(const char* repository) {
    if (!repository || repository[0] == '\0')
        return false;
    size_t length = strnlen(repository, 257u);
    if (length == 0 || length > 256u)
        return false;

    const char* slash = strchr(repository, '/');
    if (!slash || slash == repository || slash[1] == '\0' ||
        strchr(slash + 1, '/'))
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
    if (!input)
        return NULL;
    size_t length = strnlen(input, MIRA_GITHUB_QUERY_MAX_BYTES + 1u);
    if (length > MIRA_GITHUB_QUERY_MAX_BYTES ||
        length > (SIZE_MAX - 1u) / 3u) {
        errno = EFBIG;
        return NULL;
    }

    static const char hex[] = "0123456789ABCDEF";
    char* encoded = malloc(length * 3u + 1u);
    if (!encoded)
        return NULL;
    char* output = encoded;
    for (size_t index = 0; index < length; index++) {
        unsigned char ch = (unsigned char)input[index];
        if (isalnum(ch) || ch == '-' || ch == '.' || ch == '_' || ch == '~') {
            *output++ = (char)ch;
        }
        else {
            *output++ = '%';
            *output++ = hex[ch >> 4u];
            *output++ = hex[ch & 0x0fu];
        }
    }
    *output = '\0';
    return encoded;
}

static char* github_api_url(const char* path,
                            const char* const* query,
                            int query_count) {
    if (!github_api_path_is_valid(path) || query_count < 0)
        return NULL;

    size_t capacity = strlen(MIRA_GITHUB_API_ROOT) + strlen(path) + 1u;
    char* url = malloc(capacity);
    if (!url)
        return NULL;
    snprintf(url, capacity, "%s%s", MIRA_GITHUB_API_ROOT, path);
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

static struct bx_fetch_config* github_config(
    const MiraGithubArguments* arguments,
    const char* url,
    const char* output_path) {
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;

    config->logging.verbosity = BX_FETCH_VERBOSITY_QUIET;
    config->download.show_progress = false;
    config->download.metadata_sidecars = false;
    config->download.no_proxy = arguments->no_proxy;
    config->http.paranoid = true;
    config->https.https_only = true;
    config->http.max_redirect = 10;

    free(config->download.output_document);
    config->download.output_document = strdup(output_path);
    free(config->http.redirect_method);
    config->http.redirect_method = strdup("strict");
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

    if (arguments->bearer_token) {
        if (!bx_fetch_http_bearer_token_is_valid(arguments->bearer_token)) {
            errno = EINVAL;
            bx_fetch_config_free(config);
            return NULL;
        }
        config->http.bearer_token = strdup(arguments->bearer_token);
        if (!config->http.bearer_token) {
            bx_fetch_config_free(config);
            return NULL;
        }
    }
    else if (arguments->bearer_token_file) {
        if (bx_fetch_bearer_token_load_file(
                arguments->bearer_token_file,
                &config->http.bearer_token) != 0) {
            bx_fetch_config_free(config);
            return NULL;
        }
    }
    if (config->http.bearer_token)
        config->https.require_verified_https = true;
    return config;
}

static int github_fetch(const MiraGithubArguments* arguments,
                        const char* url,
                        const char* output_path) {
    struct bx_fetch_config* config =
        github_config(arguments, url, output_path);
    if (!config) {
        github_parse_error("could not prepare GitHub request");
        return BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    int result = bx_mira_run_config(config);
    bx_fetch_config_free(config);
    return result;
}

static int read_bounded_json(const char* path, char** data_out, size_t* size_out) {
    *data_out = NULL;
    *size_out = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd == -1)
        return -1;

    struct stat status;
    if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        (uintmax_t)status.st_size > MIRA_GITHUB_JSON_MAX_BYTES ||
        (uintmax_t)status.st_size > INT_MAX) {
        int error_number = errno ? errno : EFBIG;
        close(fd);
        errno = error_number;
        return -1;
    }

    size_t size = (size_t)status.st_size;
    char* data = malloc(size + 1u);
    if (!data) {
        close(fd);
        return -1;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            int error_number = count == 0 ? EIO : errno;
            free(data);
            close(fd);
            errno = error_number;
            return -1;
        }
        offset += (size_t)count;
    }
    if (close(fd) != 0) {
        free(data);
        return -1;
    }
    data[size] = '\0';
    *data_out = data;
    *size_out = size;
    return 0;
}

static int write_stdout(const char* data, size_t length, bool* closed) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t count = write(STDOUT_FILENO, data + offset, length - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && errno == EPIPE) {
            *closed = true;
            return 1;
        }
        if (count <= 0)
            return -1;
        offset += (size_t)count;
    }
    return 0;
}

static int github_jq_output(void* userdata, jv value) {
    MiraGithubJqOutput* output = userdata;
    jv rendered = jv_dump_string(jv_copy(value), 0);
    if (!jv_is_valid(rendered) || jv_get_kind(rendered) != JV_KIND_STRING) {
        jv_free(rendered);
        return -1;
    }
    const char* text = jv_string_value(rendered);
    size_t length = (size_t)jv_string_length_bytes(jv_copy(rendered));
    int result = write_stdout(text, length, &output->downstream_closed);
    if (result == 0)
        result = write_stdout("\n", 1u, &output->downstream_closed);
    jv_free(rendered);
    return result;
}

static int filter_github_json(const char* path, const char* program) {
    char* data = NULL;
    size_t size = 0;
    if (read_bounded_json(path, &data, &size) != 0) {
        github_parse_error(
            errno == EFBIG ? "GitHub JSON response exceeds 16 MiB"
                           : "could not read GitHub JSON response");
        return BX_FETCH_EXIT_PROTOCOL;
    }

    jv input = jv_parse_sized(data, (int)size);
    free(data);
    if (!jv_is_valid(input)) {
        jv message = jv_invalid_get_msg(input);
        fprintf(stderr,
                "mira: github: invalid JSON response: %s\n",
                jv_get_kind(message) == JV_KIND_STRING
                    ? jv_string_value(message)
                    : "parse error");
        jv_free(message);
        return BX_FETCH_EXIT_PROTOCOL;
    }

    MiraGithubJqOutput output = {0};
    BxJqFilterResult filter_result;
    bx_jq_filter_result_init(&filter_result);
    BxJqFilterStatus status =
        bx_jq_filter(input, program, github_jq_output, &output, &filter_result);
    int result = BX_FETCH_EXIT_SUCCESS;
    if (status == BX_JQ_FILTER_STOPPED && output.downstream_closed) {
        result = BX_FETCH_EXIT_SUCCESS;
    }
    else if (status == BX_JQ_FILTER_COMPILE_ERROR) {
        github_parse_error("invalid --jq program");
        result = BX_FETCH_EXIT_PARSE_OR_CONFIG;
    }
    else if (status == BX_JQ_FILTER_RUNTIME_ERROR) {
        fprintf(stderr,
                "mira: github: jq filter failed: %s\n",
                jv_get_kind(filter_result.error_message) == JV_KIND_STRING
                    ? jv_string_value(filter_result.error_message)
                    : "runtime error");
        result = BX_FETCH_EXIT_PROTOCOL;
    }
    else if (status == BX_JQ_FILTER_HALTED &&
             filter_result.halt_code != 0) {
        github_parse_error("jq filter halted with an error");
        result = BX_FETCH_EXIT_PROTOCOL;
    }
    else if (status < 0) {
        github_parse_error("could not execute jq filter");
        result = BX_FETCH_EXIT_FILE_IO;
    }
    bx_jq_filter_result_clear(&filter_result);
    return result;
}

static int fetch_and_filter(const MiraGithubArguments* arguments,
                            const char* url,
                            const char* program) {
    char path[] = "/tmp/mira-github.XXXXXX";
    int fd = mkstemp(path);
    if (fd == -1) {
        github_parse_error("could not create bounded response file");
        return BX_FETCH_EXIT_FILE_IO;
    }
    if (close(fd) != 0) {
        unlink(path);
        github_parse_error("could not prepare bounded response file");
        return BX_FETCH_EXIT_FILE_IO;
    }

    int result = github_fetch(arguments, url, path);
    if (result == BX_FETCH_EXIT_SUCCESS)
        result = filter_github_json(path, program);
    if (unlink(path) != 0 && errno != ENOENT &&
        result == BX_FETCH_EXIT_SUCCESS)
        result = BX_FETCH_EXIT_FILE_IO;
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
        arguments->operands[0],
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
        !github_repo_is_valid(arguments->operands[0])) {
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
    char* url = github_api_url("/search/issues", query_arguments, 1);
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
    if (strcmp(argv[1], "api") == 0)
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
