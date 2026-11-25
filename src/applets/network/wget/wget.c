#define _GNU_SOURCE
#include "applets.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/http_status.h"
#include "lib/option_scan.h"
#include "lib/size_parse.h"
#include "wget.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum {
    WGET_OPT_NO_VERBOSE = 1000,
    WGET_OPT_NO_DIRECTORIES,
    WGET_OPT_RETRY_CONNREFUSED,
    WGET_OPT_RETRY_ON_HTTP_ERROR,
    WGET_OPT_SPIDER,
    WGET_OPT_DNS_TIMEOUT,
    WGET_OPT_CONNECT_TIMEOUT,
    WGET_OPT_READ_TIMEOUT,
    WGET_OPT_WAITRETRY,
    WGET_OPT_RANDOM_WAIT,
    WGET_OPT_NO_PROXY,
    WGET_OPT_BIND_ADDRESS,
    WGET_OPT_LIMIT_RATE,
    WGET_OPT_NO_DNS_CACHE,
    WGET_OPT_USER,
    WGET_OPT_PASSWORD,
    WGET_OPT_UNLINK,
    WGET_OPT_XATTR,
    WGET_OPT_NO_HOST_DIRECTORIES,
    WGET_OPT_PROTOCOL_DIRECTORIES,
    WGET_OPT_CUT_DIRS,
    WGET_OPT_HTTP_USER,
    WGET_OPT_HTTP_PASSWORD,
    WGET_OPT_HEADER,
    WGET_OPT_MAX_REDIRECT,
    WGET_OPT_PROXY_USER,
    WGET_OPT_PROXY_PASSWORD,
    WGET_OPT_REFERER,
    WGET_OPT_SAVE_HEADERS,
    WGET_OPT_NO_HTTP_KEEP_ALIVE,
    WGET_OPT_NO_COOKIES,
    WGET_OPT_LOAD_COOKIES,
    WGET_OPT_SAVE_COOKIES,
    WGET_OPT_KEEP_SESSION_COOKIES,
    WGET_OPT_POST_DATA,
    WGET_OPT_POST_FILE,
    WGET_OPT_METHOD,
    WGET_OPT_AUTH_NO_CHALLENGE,
    WGET_OPT_HTTPS_ONLY,
    WGET_OPT_NO_CHECK_CERTIFICATE,
    WGET_OPT_CERTIFICATE,
    WGET_OPT_PRIVATE_KEY,
    WGET_OPT_CA_CERTIFICATE,
    WGET_OPT_CA_DIRECTORY,
    WGET_OPT_PINNEDPUBKEY,
    WGET_OPT_NO_HSTS,
    WGET_OPT_HSTS_FILE,
};

static const struct bx_option_spec wget_options[] = {
    {'V', 'V', "version", BX_OPTION_NO_VALUE},
    {'h', 'h', "help", BX_OPTION_NO_VALUE},
    {'q', 'q', "quiet", BX_OPTION_NO_VALUE},
    {'v', 'v', "verbose", BX_OPTION_NO_VALUE},
    {WGET_OPT_NO_VERBOSE, 0, "no-verbose", BX_OPTION_NO_VALUE},
    {'t', 't', "tries", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_RETRY_CONNREFUSED, 0, "retry-connrefused", BX_OPTION_NO_VALUE},
    {WGET_OPT_RETRY_ON_HTTP_ERROR, 0, "retry-on-http-error", BX_OPTION_REQUIRED_VALUE},
    {'O', 'O', "output-document", BX_OPTION_REQUIRED_VALUE},
    {'c', 'c', "continue", BX_OPTION_NO_VALUE},
    {'N', 'N', "timestamping", BX_OPTION_NO_VALUE},
    {'S', 'S', "server-response", BX_OPTION_NO_VALUE},
    {WGET_OPT_SPIDER, 0, "spider", BX_OPTION_NO_VALUE},
    {'T', 'T', "timeout", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_DNS_TIMEOUT, 0, "dns-timeout", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_CONNECT_TIMEOUT, 0, "connect-timeout", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_READ_TIMEOUT, 0, "read-timeout", BX_OPTION_REQUIRED_VALUE},
    {'w', 'w', "wait", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_WAITRETRY, 0, "waitretry", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_RANDOM_WAIT, 0, "random-wait", BX_OPTION_NO_VALUE},
    {'Y', 'Y', "proxy", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_NO_PROXY, 0, "no-proxy", BX_OPTION_NO_VALUE},
    {'Q', 'Q', "quota", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_BIND_ADDRESS, 0, "bind-address", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_LIMIT_RATE, 0, "limit-rate", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_NO_DNS_CACHE, 0, "no-dns-cache", BX_OPTION_NO_VALUE},
    {'4', '4', "inet4-only", BX_OPTION_NO_VALUE},
    {'6', '6', "inet6-only", BX_OPTION_NO_VALUE},
    {WGET_OPT_USER, 0, "user", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_PASSWORD, 0, "password", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_UNLINK, 0, "unlink", BX_OPTION_NO_VALUE},
    {WGET_OPT_XATTR, 0, "xattr", BX_OPTION_NO_VALUE},
    {WGET_OPT_NO_DIRECTORIES, 0, "no-directories", BX_OPTION_NO_VALUE},
    {'x', 'x', "force-directories", BX_OPTION_NO_VALUE},
    {WGET_OPT_NO_HOST_DIRECTORIES, 0, "no-host-directories", BX_OPTION_NO_VALUE},
    {WGET_OPT_PROTOCOL_DIRECTORIES, 0, "protocol-directories", BX_OPTION_NO_VALUE},
    {'P', 'P', "directory-prefix", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_CUT_DIRS, 0, "cut-dirs", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_HTTP_USER, 0, "http-user", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_HTTP_PASSWORD, 0, "http-password", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_HEADER, 0, "header", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_MAX_REDIRECT, 0, "max-redirect", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_PROXY_USER, 0, "proxy-user", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_PROXY_PASSWORD, 0, "proxy-password", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_REFERER, 0, "referer", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_SAVE_HEADERS, 0, "save-headers", BX_OPTION_NO_VALUE},
    {'U', 'U', "user-agent", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_NO_HTTP_KEEP_ALIVE, 0, "no-http-keep-alive", BX_OPTION_NO_VALUE},
    {WGET_OPT_NO_COOKIES, 0, "no-cookies", BX_OPTION_NO_VALUE},
    {WGET_OPT_LOAD_COOKIES, 0, "load-cookies", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_SAVE_COOKIES, 0, "save-cookies", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_KEEP_SESSION_COOKIES, 0, "keep-session-cookies", BX_OPTION_NO_VALUE},
    {WGET_OPT_POST_DATA, 0, "post-data", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_POST_FILE, 0, "post-file", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_METHOD, 0, "method", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_AUTH_NO_CHALLENGE, 0, "auth-no-challenge", BX_OPTION_NO_VALUE},
    {WGET_OPT_HTTPS_ONLY, 0, "https-only", BX_OPTION_NO_VALUE},
    {WGET_OPT_NO_CHECK_CERTIFICATE, 0, "no-check-certificate", BX_OPTION_NO_VALUE},
    {WGET_OPT_CERTIFICATE, 0, "certificate", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_PRIVATE_KEY, 0, "private-key", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_CA_CERTIFICATE, 0, "ca-certificate", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_CA_DIRECTORY, 0, "ca-directory", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_PINNEDPUBKEY, 0, "pinnedpubkey", BX_OPTION_REQUIRED_VALUE},
    {WGET_OPT_NO_HSTS, 0, "no-hsts", BX_OPTION_NO_VALUE},
    {WGET_OPT_HSTS_FILE, 0, "hsts-file", BX_OPTION_REQUIRED_VALUE},
    {'n', 'n', NULL, BX_OPTION_OPTIONAL_VALUE},
};

static void wget_usage_hint(FILE* stream) {
    fputs(
        "Usage: wget [OPTION]... [URL]...\n\n"
        "Try `wget --help' for more options.\n",
        stream);
}

static void wget_parse_error(const char* summary, int exit_code) {
    fprintf(stderr, "wget: %s\n", summary);
    if (exit_code == 1 || exit_code == 2)
        wget_usage_hint(stderr);
}

static int wget_replace_string(char** destination, const char* value) {
    char* replacement = strdup(value);
    if (!replacement)
        return -1;
    free(*destination);
    *destination = replacement;
    return 0;
}

static bool wget_parse_nonnegative_int(const char* text, int* value_out) {
    uintmax_t value = 0;
    if (!bx_size_parse_uint(text, &value) || value > (uintmax_t)INT_MAX)
        return false;
    *value_out = (int)value;
    return true;
}

static bool wget_parse_positive_int(const char* text, int* value_out) {
    return wget_parse_nonnegative_int(text, value_out) && *value_out > 0;
}

static bool wget_parse_scaled_long(const char* text, long* value_out) {
    uintmax_t value = 0;
    if (!bx_size_parse_scaled_uint(text, &value) || value > (uintmax_t)LONG_MAX)
        return false;
    *value_out = (long)value;
    return true;
}

static bool wget_parse_scaled_i64(const char* text, int64_t* value_out) {
    uintmax_t value = 0;
    if (!bx_size_parse_scaled_uint(text, &value) || value > (uintmax_t)INT64_MAX)
        return false;
    *value_out = (int64_t)value;
    return true;
}

static bool wget_retry_statuses_valid(const char* list) {
    if (!list || !list[0])
        return false;
    const char* cursor = list;
    while (*cursor) {
        const char* token = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        int status = 0;
        if (!bx_fetch_http_status_parse_token(token, (size_t)(cursor - token), &status))
            return false;
        if (*cursor == ',') {
            cursor++;
            if (!*cursor)
                return false;
        }
    }
    return true;
}

#define WGET_SET_STRING(field)                        \
    do {                                              \
        if (wget_replace_string(&(field), option.value)) \
            goto allocation_failure;                  \
    } while (0)

#define WGET_PARSE_INT(field, name)                                      \
    do {                                                                 \
        if (!wget_parse_nonnegative_int(option.value, &(field))) {        \
            fprintf(stderr, "wget: --%s: invalid numeric value\n", name); \
            goto parse_failure;                                          \
        }                                                                \
    } while (0)

static struct bx_fetch_config* wget_parse_cli(int argc, char** argv, int* exit_code_out) {
    *exit_code_out = 2;
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;
    char** urls = NULL;
    config->logging.structured_errors = false;
    config->logging.verbosity = BX_FETCH_VERBOSITY_VERBOSE;
    config->download.show_progress = true;
    config->download.max_threads = 1;
    config->download.metadata_sidecars = false;
    if (wget_replace_string(&config->http.user_agent, "Wget/1.25.0") != 0)
        goto allocation_failure;

    urls = calloc((size_t)argc, sizeof(*urls));
    if (urls == NULL)
        goto allocation_failure;
    int url_count = 0;
    struct bx_option_cursor cursor;
    bx_option_cursor_init(&cursor, argv, 1, (size_t)argc, BX_OPTION_ORDERED | BX_OPTION_ABBREVIATE,
                          wget_options, sizeof(wget_options) / sizeof(wget_options[0]));
    struct bx_option_result option;
    while ((option = bx_option_next(&cursor)).event != BX_OPTION_END) {
        if (option.event == BX_OPTION_OPERAND) {
            if (option.token[0] == '\0') {
                wget_parse_error("empty URL operand", *exit_code_out);
                goto parse_failure;
            }
            urls[url_count++] = argv[option.index];
            continue;
        }
        if (option.event != BX_OPTION_MATCH) {
            const char* reason = option.event == BX_OPTION_MISSING_VALUE ? "option requires an argument" :
                option.event == BX_OPTION_AMBIGUOUS ? "ambiguous option" : "unrecognized option";
            fprintf(stderr, "wget: %s '%s'\n", reason, option.token);
            wget_usage_hint(stderr);
            goto parse_failure;
        }
        switch (option.spec->id) {
            case 'V':
                config->startup.show_version = true;
                break;
            case 'h':
                config->startup.show_help = true;
                break;
            case 'q':
                config->logging.verbosity = BX_FETCH_VERBOSITY_QUIET;
                config->download.show_progress = false;
                break;
            case 'v':
                config->logging.verbosity = BX_FETCH_VERBOSITY_VERBOSE;
                break;
            case 'n': {
                const char* token = option.token;
                if (strcmp(token, "-nd") == 0)
                    config->dirs.no_directories = true;
                else if (strcmp(token, "-nH") == 0)
                    config->dirs.no_host_directories = true;
                else if (strcmp(token, "-nv") == 0) {
                    config->logging.verbosity = BX_FETCH_VERBOSITY_NORMAL;
                    config->download.show_progress = false;
                }
                else {
                    fprintf(stderr, "wget: unrecognized option '%s'\n", token);
                    wget_usage_hint(stderr);
                    goto parse_failure;
                }
                break;
            }
            case WGET_OPT_NO_VERBOSE:
                config->logging.verbosity = BX_FETCH_VERBOSITY_NORMAL;
                config->download.show_progress = false;
                break;
            case 't':
                if (!wget_parse_positive_int(option.value, &config->download.tries)) {
                    fputs("wget: --tries: retries must be a bounded positive integer\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_RETRY_CONNREFUSED:
                config->download.retry_connrefused = true;
                break;
            case WGET_OPT_RETRY_ON_HTTP_ERROR:
                if (!wget_retry_statuses_valid(option.value)) {
                    fputs("wget: --retry-on-http-error: invalid HTTP status list\n", stderr);
                    goto parse_failure;
                }
                WGET_SET_STRING(config->download.retry_on_http_error);
                break;
            case 'O':
                WGET_SET_STRING(config->download.output_document);
                break;
            case 'c':
                config->download.continue_download = true;
                break;
            case 'N':
                config->download.timestamping = true;
                break;
            case 'S':
                config->download.server_response = true;
                break;
            case WGET_OPT_SPIDER:
                config->download.spider = true;
                break;
            case 'T': {
                int timeout = 0;
                if (!wget_parse_nonnegative_int(option.value, &timeout)) {
                    fprintf(stderr, "wget: --timeout: Invalid time period '%s'\n", option.value);
                    goto parse_failure;
                }
                config->download.dns_timeout = timeout;
                config->download.connect_timeout = timeout;
                config->download.read_timeout = timeout;
                break;
            }
            case WGET_OPT_DNS_TIMEOUT:
                WGET_PARSE_INT(config->download.dns_timeout, "dns-timeout");
                break;
            case WGET_OPT_CONNECT_TIMEOUT:
                WGET_PARSE_INT(config->download.connect_timeout, "connect-timeout");
                break;
            case WGET_OPT_READ_TIMEOUT:
                WGET_PARSE_INT(config->download.read_timeout, "read-timeout");
                break;
            case 'w':
                WGET_PARSE_INT(config->download.wait, "wait");
                break;
            case WGET_OPT_WAITRETRY:
                WGET_PARSE_INT(config->download.waitretry, "waitretry");
                break;
            case WGET_OPT_RANDOM_WAIT:
                config->download.random_wait = true;
                break;
            case 'Y':
                if (strcasecmp(option.value, "off") == 0)
                    config->download.no_proxy = true;
                else if (strcasecmp(option.value, "on") == 0)
                    config->download.no_proxy = false;
                else {
                    fputs("wget: --proxy: expected `on' or `off'\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_NO_PROXY:
                config->download.no_proxy = true;
                break;
            case 'Q':
                if (!wget_parse_scaled_long(option.value, &config->download.quota)) {
                    fputs("wget: --quota: invalid byte count\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_BIND_ADDRESS:
                WGET_SET_STRING(config->download.bind_address);
                break;
            case WGET_OPT_LIMIT_RATE:
                if (!wget_parse_scaled_i64(option.value, &config->download.limit_rate_bytes_per_sec) || config->download.limit_rate_bytes_per_sec <= 0) {
                    fputs("wget: --limit-rate: invalid byte rate\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_NO_DNS_CACHE:
                config->download.no_dns_cache = true;
                break;
            case '4':
                config->download.inet4_only = true;
                break;
            case '6':
                config->download.inet6_only = true;
                break;
            case WGET_OPT_USER:
                WGET_SET_STRING(config->download.user);
                break;
            case WGET_OPT_PASSWORD:
                WGET_SET_STRING(config->download.password);
                break;
            case WGET_OPT_UNLINK:
                config->download.unlink = true;
                break;
            case WGET_OPT_XATTR:
                config->download.xattr = true;
                break;
            case WGET_OPT_NO_DIRECTORIES:
                config->dirs.no_directories = true;
                break;
            case 'x':
                config->dirs.force_directories = true;
                break;
            case WGET_OPT_NO_HOST_DIRECTORIES:
                config->dirs.no_host_directories = true;
                break;
            case WGET_OPT_PROTOCOL_DIRECTORIES:
                config->dirs.protocol_directories = true;
                break;
            case 'P':
                WGET_SET_STRING(config->dirs.directory_prefix);
                break;
            case WGET_OPT_CUT_DIRS:
                WGET_PARSE_INT(config->dirs.cut_dirs, "cut-dirs");
                break;
            case WGET_OPT_HTTP_USER:
                WGET_SET_STRING(config->http.http_user);
                break;
            case WGET_OPT_HTTP_PASSWORD:
                WGET_SET_STRING(config->http.http_password);
                break;
            case WGET_OPT_HEADER:
                if (bx_fetch_config_add_http_header(config, option.value) != BX_FETCH_HTTP_HEADER_OK) {
                    fputs("wget: --header: invalid or forbidden HTTP header\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_MAX_REDIRECT:
                WGET_PARSE_INT(config->http.max_redirect, "max-redirect");
                break;
            case WGET_OPT_PROXY_USER:
                WGET_SET_STRING(config->http.proxy_user);
                break;
            case WGET_OPT_PROXY_PASSWORD:
                WGET_SET_STRING(config->http.proxy_password);
                break;
            case WGET_OPT_REFERER:
                WGET_SET_STRING(config->http.referer);
                break;
            case WGET_OPT_SAVE_HEADERS:
                config->http.save_headers = true;
                break;
            case 'U':
                WGET_SET_STRING(config->http.user_agent);
                break;
            case WGET_OPT_NO_HTTP_KEEP_ALIVE:
                config->http.no_http_keep_alive = true;
                break;
            case WGET_OPT_NO_COOKIES:
                config->http.no_cookies = true;
                break;
            case WGET_OPT_LOAD_COOKIES:
                WGET_SET_STRING(config->http.load_cookies);
                break;
            case WGET_OPT_SAVE_COOKIES:
                WGET_SET_STRING(config->http.save_cookies);
                break;
            case WGET_OPT_KEEP_SESSION_COOKIES:
                config->http.keep_session_cookies = true;
                break;
            case WGET_OPT_POST_DATA:
                WGET_SET_STRING(config->http.post_data);
                break;
            case WGET_OPT_POST_FILE:
                WGET_SET_STRING(config->http.post_file);
                break;
            case WGET_OPT_METHOD:
                if (!bx_fetch_http_method_is_valid(option.value)) {
                    fputs("wget: --method: invalid HTTP method\n", stderr);
                    goto parse_failure;
                }
                WGET_SET_STRING(config->http.method);
                break;
            case WGET_OPT_AUTH_NO_CHALLENGE:
                config->http.auth_no_challenge = true;
                break;
            case WGET_OPT_HTTPS_ONLY:
                config->https.https_only = true;
                break;
            case WGET_OPT_NO_CHECK_CERTIFICATE:
                config->https.no_check_certificate = true;
                break;
            case WGET_OPT_CERTIFICATE:
                WGET_SET_STRING(config->https.certificate);
                break;
            case WGET_OPT_PRIVATE_KEY:
                WGET_SET_STRING(config->https.private_key);
                break;
            case WGET_OPT_CA_CERTIFICATE:
                WGET_SET_STRING(config->https.ca_certificate);
                break;
            case WGET_OPT_CA_DIRECTORY:
                WGET_SET_STRING(config->https.ca_directory);
                break;
            case WGET_OPT_PINNEDPUBKEY:
                WGET_SET_STRING(config->https.pinnedpubkey);
                break;
            case WGET_OPT_NO_HSTS:
                config->https.no_hsts = true;
                break;
            case WGET_OPT_HSTS_FILE:
                WGET_SET_STRING(config->https.hsts_file);
                break;
            default:
                fputs("wget: unsupported option\n", stderr);
                goto parse_failure;
        }
    }

    if (bx_fetch_config_copy_urls(config, url_count, urls) != 0)
        goto allocation_failure;
    if (config->download.inet4_only && config->download.inet6_only) {
        fputs("Cannot specify both --inet4-only and --inet6-only.\n", stderr);
        *exit_code_out = 1;
        goto parse_failure;
    }
    if (config->http.post_data && config->http.post_file) {
        fputs("wget: --post-data and --post-file are mutually exclusive\n", stderr);
        goto parse_failure;
    }
    if (config->download.output_document && config->download.output_document[0] == '\0') {
        fputs("wget: --output-document cannot be empty\n", stderr);
        goto parse_failure;
    }
    if (config->download.output_document && config->input.url_count > 1 && strcmp(config->download.output_document, "-") != 0) {
        fputs("wget: multiple URLs with a file --output-document are not supported\n", stderr);
        goto parse_failure;
    }
    if (config->download.spider && config->download.output_document) {
        fputs("wget: --spider cannot be combined with --output-document\n", stderr);
        goto parse_failure;
    }
    if (config->http.no_cookies && (config->http.load_cookies || config->http.save_cookies || config->http.keep_session_cookies)) {
        fputs("wget: --no-cookies cannot be combined with cookie file options\n", stderr);
        goto parse_failure;
    }
    free(urls);
    return config;

allocation_failure:
    fputs("wget: memory exhausted while parsing command line\n", stderr);
    *exit_code_out = 1;
parse_failure:
    free(urls);
    bx_fetch_config_free(config);
    return NULL;
}

#undef WGET_SET_STRING
#undef WGET_PARSE_INT

static void wget_print_help(void) {
    fputs(
        "GNU Wget compatible network retriever built into bx.\n"
        "Usage: wget [OPTION]... [URL]...\n\n"
        "Startup:\n"
        "  -V, --version                 display version information\n"
        "  -h, --help                    display this help\n"
        "  -q, --quiet                   suppress normal output\n"
        "  -v, --verbose                 verbose output\n"
        "      --no-verbose              normal output without progress\n\n"
        "Download:\n"
        "  -t, --tries=NUMBER            bounded positive attempt count\n"
        "  -O, --output-document=FILE    write to FILE (`-' means stdout)\n"
        "  -c, --continue                resume a partial file\n"
        "  -N, --timestamping            retrieve only newer files\n"
        "  -S, --server-response         print response headers\n"
        "      --spider                  check existence without saving\n"
        "  -T, --timeout=SECONDS         set all network timeouts\n"
        "  -w, --wait=SECONDS            wait between requests\n"
        "  -Q, --quota=NUMBER            set download quota\n"
        "      --limit-rate=RATE         limit aggregate transfer rate\n"
        "      --no-proxy                disable proxy use\n"
        "  -4, --inet4-only              use IPv4 only\n"
        "  -6, --inet6-only              use IPv6 only\n\n"
        "Directories:\n"
        "  -nd, --no-directories         do not create directory hierarchy\n"
        "  -P, --directory-prefix=DIR    save below DIR\n"
        "  -x, --force-directories       force directory hierarchy\n\n"
        "HTTP/TLS:\n"
        "      --header=LINE             add a validated request header\n"
        "  -U, --user-agent=AGENT        set User-Agent\n"
        "      --http-user=USER          set HTTP user\n"
        "      --http-password=PASS      set HTTP password\n"
        "      --no-check-certificate    disable certificate verification\n"
        "      --ca-certificate=FILE     use CA bundle\n"
        "      --pinnedpubkey=PIN        require a public-key pin\n"
        "      --https-only              reject plaintext HTTP\n\n"
        "Unsupported options fail explicitly; they are never accepted and ignored.\n",
        stdout);
}

int bx_wget_main(int argc, char** argv) {
    int parse_exit = 2;
    struct bx_fetch_config* config = wget_parse_cli(argc, argv, &parse_exit);
    if (!config)
        return parse_exit;

    int result = BX_FETCH_EXIT_SUCCESS;
    if (config->startup.show_version) {
        printf("GNU Wget compatible (bx) %s\n", BX_VERSION);
    }
    else if (config->startup.show_help) {
        wget_print_help();
    }
    else if (config->input.url_count == 0) {
        wget_parse_error("missing URL", 1);
        result = 1;
    }
    else {
        result = bx_wget_run_config(config);
    }

    bx_fetch_config_free(config);
    return result;
}
