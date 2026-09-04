#define _GNU_SOURCE
#include "applets.h"
#include "lib/fetch/exit_code.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/http_status.h"
#include "lib/size_parse.h"
#include "wget.h"
#include <errno.h>
#include <getopt.h>
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
    WGET_OPT_FORCE_DIRECTORIES,
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

static const struct option wget_options[] = {
    {"version", no_argument, NULL, 'V'},
    {"help", no_argument, NULL, 'h'},
    {"quiet", no_argument, NULL, 'q'},
    {"verbose", no_argument, NULL, 'v'},
    {"no-verbose", no_argument, NULL, WGET_OPT_NO_VERBOSE},
    {"tries", required_argument, NULL, 't'},
    {"retry-connrefused", no_argument, NULL, WGET_OPT_RETRY_CONNREFUSED},
    {"retry-on-http-error", required_argument, NULL, WGET_OPT_RETRY_ON_HTTP_ERROR},
    {"output-document", required_argument, NULL, 'O'},
    {"continue", no_argument, NULL, 'c'},
    {"timestamping", no_argument, NULL, 'N'},
    {"server-response", no_argument, NULL, 'S'},
    {"spider", no_argument, NULL, WGET_OPT_SPIDER},
    {"timeout", required_argument, NULL, 'T'},
    {"dns-timeout", required_argument, NULL, WGET_OPT_DNS_TIMEOUT},
    {"connect-timeout", required_argument, NULL, WGET_OPT_CONNECT_TIMEOUT},
    {"read-timeout", required_argument, NULL, WGET_OPT_READ_TIMEOUT},
    {"wait", required_argument, NULL, 'w'},
    {"waitretry", required_argument, NULL, WGET_OPT_WAITRETRY},
    {"random-wait", no_argument, NULL, WGET_OPT_RANDOM_WAIT},
    {"proxy", required_argument, NULL, 'Y'},
    {"no-proxy", no_argument, NULL, WGET_OPT_NO_PROXY},
    {"quota", required_argument, NULL, 'Q'},
    {"bind-address", required_argument, NULL, WGET_OPT_BIND_ADDRESS},
    {"limit-rate", required_argument, NULL, WGET_OPT_LIMIT_RATE},
    {"no-dns-cache", no_argument, NULL, WGET_OPT_NO_DNS_CACHE},
    {"inet4-only", no_argument, NULL, '4'},
    {"inet6-only", no_argument, NULL, '6'},
    {"user", required_argument, NULL, WGET_OPT_USER},
    {"password", required_argument, NULL, WGET_OPT_PASSWORD},
    {"unlink", no_argument, NULL, WGET_OPT_UNLINK},
    {"xattr", no_argument, NULL, WGET_OPT_XATTR},
    {"no-directories", no_argument, NULL, WGET_OPT_NO_DIRECTORIES},
    {"force-directories", no_argument, NULL, WGET_OPT_FORCE_DIRECTORIES},
    {"no-host-directories", no_argument, NULL, WGET_OPT_NO_HOST_DIRECTORIES},
    {"protocol-directories", no_argument, NULL, WGET_OPT_PROTOCOL_DIRECTORIES},
    {"directory-prefix", required_argument, NULL, 'P'},
    {"cut-dirs", required_argument, NULL, WGET_OPT_CUT_DIRS},
    {"http-user", required_argument, NULL, WGET_OPT_HTTP_USER},
    {"http-password", required_argument, NULL, WGET_OPT_HTTP_PASSWORD},
    {"header", required_argument, NULL, WGET_OPT_HEADER},
    {"max-redirect", required_argument, NULL, WGET_OPT_MAX_REDIRECT},
    {"proxy-user", required_argument, NULL, WGET_OPT_PROXY_USER},
    {"proxy-password", required_argument, NULL, WGET_OPT_PROXY_PASSWORD},
    {"referer", required_argument, NULL, WGET_OPT_REFERER},
    {"save-headers", no_argument, NULL, WGET_OPT_SAVE_HEADERS},
    {"user-agent", required_argument, NULL, 'U'},
    {"no-http-keep-alive", no_argument, NULL, WGET_OPT_NO_HTTP_KEEP_ALIVE},
    {"no-cookies", no_argument, NULL, WGET_OPT_NO_COOKIES},
    {"load-cookies", required_argument, NULL, WGET_OPT_LOAD_COOKIES},
    {"save-cookies", required_argument, NULL, WGET_OPT_SAVE_COOKIES},
    {"keep-session-cookies", no_argument, NULL, WGET_OPT_KEEP_SESSION_COOKIES},
    {"post-data", required_argument, NULL, WGET_OPT_POST_DATA},
    {"post-file", required_argument, NULL, WGET_OPT_POST_FILE},
    {"method", required_argument, NULL, WGET_OPT_METHOD},
    {"auth-no-challenge", no_argument, NULL, WGET_OPT_AUTH_NO_CHALLENGE},
    {"https-only", no_argument, NULL, WGET_OPT_HTTPS_ONLY},
    {"no-check-certificate", no_argument, NULL, WGET_OPT_NO_CHECK_CERTIFICATE},
    {"certificate", required_argument, NULL, WGET_OPT_CERTIFICATE},
    {"private-key", required_argument, NULL, WGET_OPT_PRIVATE_KEY},
    {"ca-certificate", required_argument, NULL, WGET_OPT_CA_CERTIFICATE},
    {"ca-directory", required_argument, NULL, WGET_OPT_CA_DIRECTORY},
    {"pinnedpubkey", required_argument, NULL, WGET_OPT_PINNEDPUBKEY},
    {"no-hsts", no_argument, NULL, WGET_OPT_NO_HSTS},
    {"hsts-file", required_argument, NULL, WGET_OPT_HSTS_FILE},
    {NULL, 0, NULL, 0},
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

#define WGET_SET_STRING(field)                     \
    do {                                           \
        if (wget_replace_string(&(field), optarg)) \
            goto allocation_failure;               \
    } while (0)

#define WGET_PARSE_INT(field, name)                                       \
    do {                                                                  \
        if (!wget_parse_nonnegative_int(optarg, &(field))) {              \
            fprintf(stderr, "wget: --%s: invalid numeric value\n", name); \
            goto parse_failure;                                           \
        }                                                                 \
    } while (0)

static struct bx_fetch_config* wget_parse_cli(int argc, char** argv, int* exit_code_out) {
    *exit_code_out = 2;
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;
    config->logging.structured_errors = false;
    config->logging.verbosity = BX_FETCH_VERBOSITY_VERBOSE;
    config->download.show_progress = true;
    config->download.max_threads = 1;
    config->download.metadata_sidecars = false;
    if (wget_replace_string(&config->http.user_agent, "Wget/1.25.0") != 0)
        goto allocation_failure;

    opterr = 0;
    optind = 0;
    for (;;) {
        int option = getopt_long(argc, argv, "Vhqvn:t:O:cNST:w:Y:Q:46P:U:x", wget_options, NULL);
        if (option == -1)
            break;
        switch (option) {
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
                const char* token = optind > 0 && optind <= argc ? argv[optind - 1] : "";
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
                if (!wget_parse_positive_int(optarg, &config->download.tries)) {
                    fputs("wget: --tries: retries must be a bounded positive integer\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_RETRY_CONNREFUSED:
                config->download.retry_connrefused = true;
                break;
            case WGET_OPT_RETRY_ON_HTTP_ERROR:
                if (!wget_retry_statuses_valid(optarg)) {
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
                if (!wget_parse_nonnegative_int(optarg, &timeout)) {
                    fprintf(stderr, "wget: --timeout: Invalid time period '%s'\n", optarg);
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
                if (strcasecmp(optarg, "off") == 0)
                    config->download.no_proxy = true;
                else if (strcasecmp(optarg, "on") == 0)
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
                if (!wget_parse_scaled_long(optarg, &config->download.quota)) {
                    fputs("wget: --quota: invalid byte count\n", stderr);
                    goto parse_failure;
                }
                break;
            case WGET_OPT_BIND_ADDRESS:
                WGET_SET_STRING(config->download.bind_address);
                break;
            case WGET_OPT_LIMIT_RATE:
                if (!wget_parse_scaled_i64(optarg, &config->download.limit_rate_bytes_per_sec) || config->download.limit_rate_bytes_per_sec <= 0) {
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
            case WGET_OPT_FORCE_DIRECTORIES:
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
                if (bx_fetch_config_add_http_header(config, optarg) != BX_FETCH_HTTP_HEADER_OK) {
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
                if (!bx_fetch_http_method_is_valid(optarg)) {
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
            case '?': {
                const char* token = optind > 0 && optind <= argc ? argv[optind - 1] : "";
                fprintf(stderr, "wget: unrecognized option '%s'\n", token);
                wget_usage_hint(stderr);
                goto parse_failure;
            }
            default:
                fputs("wget: unsupported option\n", stderr);
                goto parse_failure;
        }
    }

    if (bx_fetch_config_copy_urls(config, argc - optind, &argv[optind]) != 0)
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
    return config;

allocation_failure:
    fputs("wget: memory exhausted while parsing command line\n", stderr);
    *exit_code_out = 1;
parse_failure:
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
