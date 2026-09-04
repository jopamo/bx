#define _GNU_SOURCE
#include "mira.h"
#include "options.h"
#include "lib/fetch/error.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/http_status.h"
#include "lib/fetch/regex.h"
#include "lib/size_parse.h"
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    bool timeout;
    bool dns_timeout;
    bool connect_timeout;
    bool read_timeout;
} MiraTimeoutPresence;

void bx_mira_emit_parse_error(const struct bx_fetch_config* config, const char* summary) {
    fprintf(stderr, "mira: %s\n", summary);
    if (!config || config->logging.structured_errors)
        bx_fetch_error_emit_simple(stderr, BX_FETCH_ERROR_CLASS_PARSE, summary, NULL, NULL, -1, -1);
}

static void mira_emit_allocation_error(const struct bx_fetch_config* config) {
    const char* summary = "out of memory while parsing command line";
    fprintf(stderr, "mira: %s\n", summary);
    if (!config || config->logging.structured_errors)
        bx_fetch_error_emit_simple(stderr, BX_FETCH_ERROR_CLASS_INTERNAL, summary, NULL, NULL, -1, ENOMEM);
}

static void mira_parse_errorf(const struct bx_fetch_config* config, const char* format, ...) {
    char summary[512];
    va_list args;
    va_start(args, format);
    int written = vsnprintf(summary, sizeof(summary), format, args);
    va_end(args);
    bx_mira_emit_parse_error(config, written >= 0 && (size_t)written < sizeof(summary) ? summary : "command-line parse error");
}

static int mira_replace_string(char** destination, const char* value) {
    char* replacement = NULL;
    if (value && value[0] != '\0') {
        replacement = strdup(value);
        if (!replacement)
            return -1;
    }
    free(*destination);
    *destination = replacement;
    return 0;
}

static bool mira_parse_int(const char* value, int minimum, int* output) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_uint(value, &parsed) || parsed > (uintmax_t)INT_MAX || parsed < (uintmax_t)minimum) {
        return false;
    }
    *output = (int)parsed;
    return true;
}

static bool mira_parse_scaled_long(const char* value, long* output) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_scaled_uint(value, &parsed) || parsed > (uintmax_t)LONG_MAX) {
        return false;
    }
    *output = (long)parsed;
    return true;
}

static bool mira_parse_scaled_i64(const char* value, int64_t* output) {
    uintmax_t parsed = 0;
    if (!bx_size_parse_scaled_uint(value, &parsed) || parsed > (uintmax_t)INT64_MAX || parsed == 0) {
        return false;
    }
    *output = (int64_t)parsed;
    return true;
}

static bool mira_retry_statuses_valid(const char* list) {
    if (!list || list[0] == '\0')
        return false;
    const char* cursor = list;
    while (*cursor) {
        const char* token = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        int status = 0;
        if (!bx_fetch_http_status_parse_token(token, (size_t)(cursor - token), &status)) {
            return false;
        }
        if (*cursor == ',') {
            cursor++;
            if (*cursor == '\0')
                return false;
        }
    }
    return true;
}

/* Returns one for valid input, zero for invalid input, and -1 on allocation failure. */
static int mira_restrict_names_valid(const char* value) {
    if (!value || value[0] == '\0')
        return 0;
    char* copy = strdup(value);
    if (!copy)
        return -1;
    bool valid = true;
    bool found = false;
    char* save = NULL;
    for (char* token = strtok_r(copy, ",", &save); token; token = strtok_r(NULL, ",", &save)) {
        while (*token == ' ' || *token == '\t')
            token++;
        char* end = token + strlen(token);
        while (end > token && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (token[0] == '\0')
            continue;
        found = true;
        if (strcasecmp(token, "unix") != 0 && strcasecmp(token, "windows") != 0) {
            valid = false;
            break;
        }
    }
    free(copy);
    return found && valid ? 1 : 0;
}

static int mira_set_regex_type(char** destination, const char* value) {
    int flags = 0;
    if (bx_fetch_regex_compile_flags_for_type(value, &flags) != 0) {
        errno = EINVAL;
        return -1;
    }
    (void)flags;
    return mira_replace_string(destination, strcasecmp(value, "posix-basic") == 0 ? "posix-basic" : "posix");
}

static int mira_validate_accept_regex(const struct bx_fetch_config* config) {
    if (!config->recursive.accept_regex)
        return 0;
    int flags = 0;
    if (bx_fetch_regex_compile_flags_for_type(config->recursive.regex_type, &flags) != 0) {
        errno = EINVAL;
        return -1;
    }
    regex_t expression;
    int result = regcomp(&expression, config->recursive.accept_regex, flags);
    if (result == 0)
        regfree(&expression);
    if (result != 0)
        errno = result == REG_ESPACE ? ENOMEM : EINVAL;
    return result == 0 ? 0 : -1;
}

static const char* mira_current_token(int argc, char** argv) {
    if (!argv || argc <= 1)
        return NULL;
    if (optind <= 1)
        return argv[1];
    if (optind > argc)
        return argv[argc - 1];
    return argv[optind - 1];
}

static bool mira_configured_method_is_get(const struct bx_fetch_config* config) {
    const char* method = config->http.method;
    if (!method || method[0] == '\0')
        method = config->http.post_data || config->http.post_file ? "POST" : "GET";
    return strcasecmp(method, "GET") == 0;
}

static int mira_validate_config(struct bx_fetch_config* config) {
    if (config->logging.debug_trace && !config->download.dry_run && !config->startup.show_help && !config->startup.show_version) {
        bx_mira_emit_parse_error(config, "--debug currently requires --dry-run");
        errno = EINVAL;
        return -1;
    }
    if (config->http.post_data && config->http.post_file) {
        bx_mira_emit_parse_error(config, "conflicting option tokens: --post-data and --post-file");
        errno = EINVAL;
        return -1;
    }
    if (mira_validate_accept_regex(config) != 0) {
        if (errno == ENOMEM)
            return -1;
        mira_parse_errorf(config, "invalid value for --accept-regex: %s", config->recursive.accept_regex);
        return -1;
    }
    if (config->download.output_document && (config->input.url_count != 1 || config->recursive.recursive || config->recursive.page_requisites)) {
        bx_mira_emit_parse_error(config, "--output-document requires one non-recursive URL");
        errno = EINVAL;
        return -1;
    }
    bool stdout_mode = config->download.output_document && strcmp(config->download.output_document, "-") == 0;
    if (stdout_mode && (config->download.continue_download || config->download.timestamping || config->download.unlink || config->recursive.backups > 0 || config->download.xattr)) {
        bx_mira_emit_parse_error(config, "--output-document=- conflicts with file-state options");
        errno = EINVAL;
        return -1;
    }
    if (!mira_configured_method_is_get(config) && (config->download.continue_download || config->download.timestamping)) {
        bx_mira_emit_parse_error(config, "resume and timestamping require the GET method");
        errno = EINVAL;
        return -1;
    }
    if (config->download.continue_download && config->download.unlink) {
        bx_mira_emit_parse_error(config, "conflicting option tokens: --continue and --unlink");
        errno = EINVAL;
        return -1;
    }
    if (config->http.no_cookies) {
        free(config->http.load_cookies);
        free(config->http.save_cookies);
        config->http.load_cookies = NULL;
        config->http.save_cookies = NULL;
        config->http.keep_session_cookies = false;
    }
    if (config->https.no_hsts) {
        free(config->https.hsts_file);
        config->https.hsts_file = NULL;
    }
    if (config->http.paranoid) {
        config->https.https_only = true;
        config->https.no_check_certificate = false;
        if (mira_replace_string(&config->http.redirect_method, "strict") != 0)
            return -1;
        if (config->http.max_redirect > 10)
            config->http.max_redirect = 10;
    }
    return 0;
}

#define MIRA_SET_STRING(field)                          \
    do {                                                \
        if (mira_replace_string(&(field), optarg) != 0) \
            goto allocation_failure;                    \
    } while (0)

#define MIRA_PARSE_INT(field, minimum, name)                                       \
    do {                                                                           \
        if (!mira_parse_int(optarg, minimum, &(field))) {                          \
            mira_parse_errorf(config, "invalid value for --%s: %s", name, optarg); \
            goto parse_failure;                                                    \
        }                                                                          \
    } while (0)

struct bx_fetch_config* bx_mira_parse_cli(int argc, char** argv) {
    struct bx_fetch_config* config = bx_fetch_config_new();
    if (!config)
        return NULL;
    config->download.metadata_sidecars = true;

    MiraTimeoutPresence timeout_presence = {0};
    int timeout_value = 0;
    opterr = 0;
    optind = 0;
    for (;;) {
        int option = getopt_long(argc, argv, bx_mira_short_options(), bx_mira_long_options(), NULL);
        if (option == -1)
            break;
        const MiraOptionSpec* spec = bx_mira_option_spec_for_value(option);
        if (spec && !spec->supported) {
            mira_parse_errorf(config, "unsupported option token: --%s", spec->name);
            goto parse_failure;
        }

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
            case MIRA_OPT_NO_VERBOSE:
                config->logging.verbosity = BX_FETCH_VERBOSITY_NORMAL;
                config->download.show_progress = false;
                break;
            case 'o':
                MIRA_SET_STRING(config->logging.log_file);
                config->logging.log_file_mode = BX_FETCH_LOG_FILE_TRUNCATE;
                break;
            case 'a':
                MIRA_SET_STRING(config->logging.log_file);
                config->logging.log_file_mode = BX_FETCH_LOG_FILE_APPEND;
                break;
            case MIRA_OPT_DEBUG:
                config->logging.debug_trace = true;
                break;
            case 'n': {
                const char* token = mira_current_token(argc, argv);
                if (token && strcmp(token, "-nd") == 0)
                    config->dirs.no_directories = true;
                else if (token && strcmp(token, "-nH") == 0)
                    config->dirs.no_host_directories = true;
                else if (token && strcmp(token, "-nv") == 0) {
                    config->logging.verbosity = BX_FETCH_VERBOSITY_NORMAL;
                    config->download.show_progress = false;
                }
                else if (token && strcmp(token, "-nc") == 0) {
                    bx_mira_emit_parse_error(config, "unsupported option token: --no-clobber");
                    goto parse_failure;
                }
                else if (token && strcmp(token, "-np") == 0)
                    config->recursive.no_parent = true;
                else {
                    mira_parse_errorf(config, "ambiguous short option token: %s", token ? token : "(unknown)");
                    goto parse_failure;
                }
                break;
            }
            case 't':
                MIRA_PARSE_INT(config->download.tries, 0, "tries");
                break;
            case MIRA_OPT_RETRY_CONNREFUSED:
                config->download.retry_connrefused = true;
                break;
            case MIRA_OPT_RETRY_ON_HTTP_ERROR:
                if (!mira_retry_statuses_valid(optarg)) {
                    mira_parse_errorf(config, "invalid value for --retry-on-http-error: %s", optarg);
                    goto parse_failure;
                }
                MIRA_SET_STRING(config->download.retry_on_http_error);
                break;
            case 'O':
                MIRA_SET_STRING(config->download.output_document);
                break;
            case 'c':
                config->download.continue_download = true;
                break;
            case MIRA_OPT_PROGRESS:
                if (strcasecmp(optarg, "bar") == 0)
                    config->download.show_progress = true;
                else if (strcasecmp(optarg, "none") == 0)
                    config->download.show_progress = false;
                else {
                    mira_parse_errorf(config, "invalid value for --progress: %s", optarg);
                    goto parse_failure;
                }
                break;
            case MIRA_OPT_SHOW_PROGRESS:
                config->download.show_progress = true;
                break;
            case 'N':
                config->download.timestamping = true;
                break;
            case MIRA_OPT_NO_IF_MODIFIED_SINCE:
                config->download.no_if_modified_since = true;
                break;
            case MIRA_OPT_NO_USE_SERVER_TIMESTAMPS:
                config->download.no_use_server_timestamps = true;
                break;
            case 'S':
                config->download.server_response = true;
                break;
            case MIRA_OPT_SPIDER:
                config->download.spider = true;
                break;
            case MIRA_OPT_DRY_RUN:
                config->download.dry_run = true;
                break;
            case 'T':
                MIRA_PARSE_INT(timeout_value, 0, "timeout");
                timeout_presence.timeout = true;
                break;
            case MIRA_OPT_DNS_SERVERS:
                MIRA_SET_STRING(config->download.dns_servers);
                break;
            case MIRA_OPT_BIND_DNS_ADDRESS:
                MIRA_SET_STRING(config->download.bind_dns_address);
                break;
            case MIRA_OPT_DNS_TIMEOUT:
                MIRA_PARSE_INT(config->download.dns_timeout, 0, "dns-timeout");
                timeout_presence.dns_timeout = true;
                break;
            case MIRA_OPT_CONNECT_TIMEOUT:
                MIRA_PARSE_INT(config->download.connect_timeout, 0, "connect-timeout");
                timeout_presence.connect_timeout = true;
                break;
            case MIRA_OPT_READ_TIMEOUT:
                MIRA_PARSE_INT(config->download.read_timeout, 0, "read-timeout");
                timeout_presence.read_timeout = true;
                break;
            case 'w':
                MIRA_PARSE_INT(config->download.wait, 0, "wait");
                break;
            case MIRA_OPT_WAITRETRY:
                MIRA_PARSE_INT(config->download.waitretry, 0, "waitretry");
                break;
            case MIRA_OPT_RANDOM_WAIT:
                config->download.random_wait = true;
                break;
            case MIRA_OPT_MAX_THREADS:
                MIRA_PARSE_INT(config->download.max_threads, 1, "max-threads");
                break;
            case 'Y':
                if (strcasecmp(optarg, "off") == 0)
                    config->download.no_proxy = true;
                else if (strcasecmp(optarg, "on") == 0)
                    config->download.no_proxy = false;
                else {
                    mira_parse_errorf(config, "invalid value for --proxy: %s", optarg);
                    goto parse_failure;
                }
                break;
            case MIRA_OPT_NO_PROXY:
                config->download.no_proxy = true;
                break;
            case 'Q':
                if (!mira_parse_scaled_long(optarg, &config->download.quota)) {
                    mira_parse_errorf(config, "invalid value for --quota: %s", optarg);
                    goto parse_failure;
                }
                break;
            case MIRA_OPT_BIND_ADDRESS:
                MIRA_SET_STRING(config->download.bind_address);
                break;
            case MIRA_OPT_LIMIT_RATE:
                if (!mira_parse_scaled_i64(optarg, &config->download.limit_rate_bytes_per_sec)) {
                    mira_parse_errorf(config, "invalid value for --limit-rate: %s", optarg);
                    goto parse_failure;
                }
                break;
            case MIRA_OPT_NO_DNS_CACHE:
                config->download.no_dns_cache = true;
                break;
            case MIRA_OPT_RESTRICT_FILE_NAMES: {
                int validity = mira_restrict_names_valid(optarg);
                if (validity < 0)
                    goto allocation_failure;
                if (validity == 0) {
                    mira_parse_errorf(config, "invalid value for --restrict-file-names: %s", optarg);
                    goto parse_failure;
                }
            }
                MIRA_SET_STRING(config->download.restrict_file_names);
                break;
            case '4':
                config->download.inet4_only = true;
                config->download.inet6_only = false;
                break;
            case '6':
                config->download.inet6_only = true;
                config->download.inet4_only = false;
                break;
            case MIRA_OPT_PREFER_FAMILY:
                if (strcasecmp(optarg, "none") != 0 && strcasecmp(optarg, "IPv4") != 0 && strcasecmp(optarg, "IPv6") != 0) {
                    mira_parse_errorf(config, "invalid value for --prefer-family: %s", optarg);
                    goto parse_failure;
                }
                MIRA_SET_STRING(config->download.prefer_family);
                break;
            case MIRA_OPT_USER:
                MIRA_SET_STRING(config->download.user);
                break;
            case MIRA_OPT_PASSWORD:
                MIRA_SET_STRING(config->download.password);
                break;
            case MIRA_OPT_UNLINK:
                config->download.unlink = true;
                break;
            case MIRA_OPT_XATTR:
                config->download.xattr = true;
                break;
            case MIRA_OPT_NO_DIRECTORIES:
                config->dirs.no_directories = true;
                break;
            case 'x':
                config->dirs.force_directories = true;
                break;
            case MIRA_OPT_NO_HOST_DIRECTORIES:
                config->dirs.no_host_directories = true;
                break;
            case MIRA_OPT_PROTOCOL_DIRECTORIES:
                config->dirs.protocol_directories = true;
                break;
            case 'P':
                MIRA_SET_STRING(config->dirs.directory_prefix);
                break;
            case MIRA_OPT_CUT_DIRS:
                MIRA_PARSE_INT(config->dirs.cut_dirs, 0, "cut-dirs");
                break;
            case MIRA_OPT_HTTP_USER:
                MIRA_SET_STRING(config->http.http_user);
                break;
            case MIRA_OPT_HTTP_PASSWORD:
                MIRA_SET_STRING(config->http.http_password);
                break;
            case MIRA_OPT_DEFAULT_PAGE:
                MIRA_SET_STRING(config->http.default_page);
                break;
            case MIRA_OPT_HEADER: {
                BxFetchHttpHeaderError error = bx_fetch_config_add_http_header(config, optarg);
                if (error != BX_FETCH_HTTP_HEADER_OK) {
                    mira_parse_errorf(config, "invalid value for --header: %s", bx_fetch_http_header_error_string(error));
                    goto parse_failure;
                }
                break;
            }
            case MIRA_OPT_MAX_REDIRECT:
                MIRA_PARSE_INT(config->http.max_redirect, 0, "max-redirect");
                break;
            case MIRA_OPT_REDIRECT_METHOD:
                if (strcasecmp(optarg, "legacy") != 0 && strcasecmp(optarg, "default") != 0 && strcasecmp(optarg, "strict") != 0) {
                    mira_parse_errorf(config, "invalid value for --redirect-method: %s", optarg);
                    goto parse_failure;
                }
                if (mira_replace_string(&config->http.redirect_method, strcasecmp(optarg, "strict") == 0 ? "strict" : "legacy") != 0) {
                    goto allocation_failure;
                }
                break;
            case MIRA_OPT_PARANOID:
                config->http.paranoid = true;
                break;
            case MIRA_OPT_PROXY_USER:
                MIRA_SET_STRING(config->http.proxy_user);
                break;
            case MIRA_OPT_PROXY_PASSWORD:
                MIRA_SET_STRING(config->http.proxy_password);
                break;
            case MIRA_OPT_REFERER:
                MIRA_SET_STRING(config->http.referer);
                break;
            case MIRA_OPT_SAVE_HEADERS:
                config->http.save_headers = true;
                break;
            case 'U':
                MIRA_SET_STRING(config->http.user_agent);
                break;
            case MIRA_OPT_NO_HTTP_KEEP_ALIVE:
                config->http.no_http_keep_alive = true;
                break;
            case MIRA_OPT_NO_COOKIES:
                config->http.no_cookies = true;
                break;
            case MIRA_OPT_LOAD_COOKIES:
                MIRA_SET_STRING(config->http.load_cookies);
                break;
            case MIRA_OPT_SAVE_COOKIES:
                MIRA_SET_STRING(config->http.save_cookies);
                break;
            case MIRA_OPT_KEEP_SESSION_COOKIES:
                config->http.keep_session_cookies = true;
                break;
            case MIRA_OPT_POST_DATA:
                MIRA_SET_STRING(config->http.post_data);
                break;
            case MIRA_OPT_POST_FILE:
                MIRA_SET_STRING(config->http.post_file);
                break;
            case MIRA_OPT_METHOD:
                if (!bx_fetch_http_method_is_valid(optarg)) {
                    mira_parse_errorf(config, "invalid value for --method: %s", optarg);
                    goto parse_failure;
                }
                MIRA_SET_STRING(config->http.method);
                break;
            case MIRA_OPT_AUTH_NO_CHALLENGE:
                config->http.auth_no_challenge = true;
                break;
            case MIRA_OPT_HTTPS_ONLY:
                config->https.https_only = true;
                break;
            case MIRA_OPT_NO_CHECK_CERTIFICATE:
                config->https.no_check_certificate = true;
                break;
            case MIRA_OPT_CERTIFICATE:
                MIRA_SET_STRING(config->https.certificate);
                break;
            case MIRA_OPT_PRIVATE_KEY:
                MIRA_SET_STRING(config->https.private_key);
                break;
            case MIRA_OPT_CA_CERTIFICATE:
                MIRA_SET_STRING(config->https.ca_certificate);
                break;
            case MIRA_OPT_CA_DIRECTORY:
                MIRA_SET_STRING(config->https.ca_directory);
                break;
            case MIRA_OPT_PINNEDPUBKEY:
                MIRA_SET_STRING(config->https.pinnedpubkey);
                break;
            case MIRA_OPT_NO_HSTS:
                config->https.no_hsts = true;
                break;
            case MIRA_OPT_HSTS_FILE:
                MIRA_SET_STRING(config->https.hsts_file);
                break;
            case 'r':
                config->recursive.recursive = true;
                break;
            case 'l':
                MIRA_PARSE_INT(config->recursive.level, 0, "level");
                break;
            case 'k':
                config->recursive.convert_links = true;
                break;
            case MIRA_OPT_CONVERT_FILE_ONLY:
                config->recursive.convert_file_only = true;
                break;
            case MIRA_OPT_BACKUPS:
                MIRA_PARSE_INT(config->recursive.backups, 0, "backups");
                break;
            case 'K':
                config->recursive.backup_converted = true;
                break;
            case 'm':
                config->recursive.recursive = true;
                config->download.timestamping = true;
                config->recursive.level = 0;
                break;
            case 'p':
                config->recursive.page_requisites = true;
                break;
            case 'A':
                MIRA_SET_STRING(config->recursive.accept_list);
                break;
            case 'R':
                MIRA_SET_STRING(config->recursive.reject_list);
                break;
            case MIRA_OPT_ACCEPT_REGEX:
                MIRA_SET_STRING(config->recursive.accept_regex);
                break;
            case MIRA_OPT_REGEX_TYPE:
                if (mira_set_regex_type(&config->recursive.regex_type, optarg) != 0) {
                    if (errno == ENOMEM)
                        goto allocation_failure;
                    mira_parse_errorf(config, "invalid value for --regex-type: %s", optarg);
                    goto parse_failure;
                }
                break;
            case 'D':
                MIRA_SET_STRING(config->recursive.domains);
                break;
            case MIRA_OPT_EXCLUDE_DOMAINS:
                MIRA_SET_STRING(config->recursive.exclude_domains);
                break;
            case 'H':
                config->recursive.span_hosts = true;
                break;
            case 'L':
                config->recursive.relative = true;
                break;
            case 'I':
                MIRA_SET_STRING(config->recursive.include_directories);
                break;
            case 'X':
                MIRA_SET_STRING(config->recursive.exclude_directories);
                break;
            case MIRA_OPT_NO_PARENT:
                config->recursive.no_parent = true;
                break;
            case '?':
            default: {
                const char* token = mira_current_token(argc, argv);
                mira_parse_errorf(config, "invalid option token%s%s", token ? ": " : "", token ? token : "");
                goto parse_failure;
            }
        }
    }

    if (timeout_presence.timeout) {
        if (!timeout_presence.dns_timeout)
            config->download.dns_timeout = timeout_value;
        if (!timeout_presence.connect_timeout)
            config->download.connect_timeout = timeout_value;
        if (!timeout_presence.read_timeout)
            config->download.read_timeout = timeout_value;
    }
    if (bx_fetch_config_copy_urls(config, argc - optind, argv ? &argv[optind] : NULL) != 0) {
        if (errno == EFBIG)
            bx_mira_emit_parse_error(config, "URL operands exceed the bounded URL-state contract");
        else
            goto allocation_failure;
        goto parse_failure;
    }
    errno = 0;
    if (mira_validate_config(config) != 0) {
        if (errno == ENOMEM)
            goto allocation_failure;
        goto parse_failure;
    }
    return config;

allocation_failure:
    mira_emit_allocation_error(config);
    errno = ENOMEM;
parse_failure:
    bx_fetch_config_free(config);
    return NULL;
}

#undef MIRA_SET_STRING
#undef MIRA_PARSE_INT
