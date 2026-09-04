#include "curl_capabilities.h"
#include "credentials.h"
#include "lib/fetch/url.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef BX_FETCH_HTTP2_ENABLED
#define BX_FETCH_HTTP2_ENABLED 0
#endif

static bool configured_string(const char* value) {
    return value && value[0] != '\0';
}

static int capability_failure(BxFetchNetSetupError* setup_error, const char* capability, const char* setting, CURLcode curl_code) {
    if (setup_error) {
        setup_error->present = true;
        setup_error->setting = setting;
        setup_error->capability = capability;
        setup_error->curl_code = (int)curl_code;
    }
    return -1;
}

static int require_feature(BxFetchNetSetupError* setup_error, const curl_version_info_data* version, int feature, const char* capability, const char* setting) {
    if (version && (version->features & feature) != 0)
        return 0;
    return capability_failure(setup_error, capability, setting, CURLE_NOT_BUILT_IN);
}

static bool protocol_available(const curl_version_info_data* version, const char* protocol) {
    if (!version || !version->protocols || !protocol)
        return false;

    for (const char* const* candidate = version->protocols; *candidate; candidate++) {
        if (strcasecmp(*candidate, protocol) == 0)
            return true;
    }
    return false;
}

static bool config_has_url_scheme(const struct bx_fetch_config* cfg, const char* scheme) {
    if (!cfg || !scheme)
        return false;

    if (bx_fetch_url_has_scheme(cfg->input.base_url, scheme))
        return true;
    for (int i = 0; i < cfg->input.url_count; i++) {
        if (cfg->input.urls && bx_fetch_url_has_scheme(cfg->input.urls[i], scheme)) {
            return true;
        }
    }
    return false;
}

static bool value_uses_https_proxy(const char* value) {
    return configured_string(value) && strncasecmp(value, "https://", strlen("https://")) == 0;
}

static bool environment_requests_https_proxy(const struct bx_fetch_config* cfg, bool requests_http, bool requests_https, bool requests_ftp) {
    if (!cfg || cfg->download.no_proxy)
        return false;
    const char* no_proxy = getenv("no_proxy");
    const char* upper_no_proxy = getenv("NO_PROXY");
    if ((no_proxy && strcmp(no_proxy, "*") == 0) || (upper_no_proxy && strcmp(upper_no_proxy, "*") == 0)) {
        return false;
    }

    bool unknown_request_protocol = cfg->input.input_file && cfg->input.input_file[0] != '\0';
    return ((requests_http || unknown_request_protocol) && value_uses_https_proxy(bx_fetch_net_proxy_environment_url(BX_FETCH_PROTOCOL_HTTP))) ||
           ((requests_https || unknown_request_protocol) && value_uses_https_proxy(bx_fetch_net_proxy_environment_url(BX_FETCH_PROTOCOL_HTTPS))) ||
           ((requests_ftp || unknown_request_protocol) &&
            (value_uses_https_proxy(bx_fetch_net_proxy_environment_url(BX_FETCH_PROTOCOL_FTP)) || value_uses_https_proxy(bx_fetch_net_proxy_environment_url(BX_FETCH_PROTOCOL_FTPS))));
}

static int require_protocol(BxFetchNetSetupError* setup_error, const curl_version_info_data* version, bool required, const char* protocol, const char* capability, const char* setting) {
    if (!required || protocol_available(version, protocol))
        return 0;
    return capability_failure(setup_error, capability, setting, CURLE_UNSUPPORTED_PROTOCOL);
}

#define REQUIRE_OPTION(error, curl, option, value, capability, setting)                 \
    do {                                                                                \
        CURLcode option_result = curl_easy_setopt((curl), (option), (value));           \
        if (option_result != CURLE_OK) {                                                \
            return capability_failure((error), (capability), (setting), option_result); \
        }                                                                               \
    } while (0)

int bx_fetch_net_validate_runtime_capabilities(const struct bx_fetch_config* cfg, const curl_version_info_data* version, CURL* probe, BxFetchNetSetupError* setup_error) {
    if (!cfg || !version || !probe)
        return -1;

    bool requests_http = config_has_url_scheme(cfg, "http");
    bool requests_https = config_has_url_scheme(cfg, "https");
    bool requests_ftp = config_has_url_scheme(cfg, "ftp");
    bool requests_ftps = config_has_url_scheme(cfg, "ftps");

    if (require_protocol(setup_error, version, requests_http, "http", "HTTP protocol support", "HTTP request URL") != 0 ||
        require_protocol(setup_error, version, requests_https, "https", "HTTPS protocol support", "HTTPS request URL") != 0 ||
        require_protocol(setup_error, version, requests_ftp, "ftp", "FTP protocol support", "FTP request URL") != 0 ||
        require_protocol(setup_error, version, requests_ftps, "ftps", "FTPS protocol support", "FTPS request URL") != 0) {
        return -1;
    }

    bool requests_hsts = !cfg->https.no_hsts && configured_string(cfg->https.hsts_file);
    bool requests_tls = requests_https || requests_ftps || requests_hsts || configured_string(cfg->https.pinnedpubkey) || configured_string(cfg->https.certificate) ||
                        configured_string(cfg->https.private_key) || configured_string(cfg->https.ca_certificate) || configured_string(cfg->https.ca_directory);
    if (requests_tls && require_feature(setup_error, version, CURL_VERSION_SSL, "TLS support", "TLS configuration") != 0) {
        return -1;
    }

    if (requests_hsts) {
#if LIBCURL_VERSION_NUM >= 0x074A00
        if (require_feature(setup_error, version, CURL_VERSION_HSTS, "HSTS support", "--hsts-file") != 0) {
            return -1;
        }
        REQUIRE_OPTION(setup_error, probe, CURLOPT_HSTS_CTRL, CURLHSTS_ENABLE, "HSTS enablement", "--hsts-file");
        /*
         * Probe option support without attaching the user's cache file:
         * cleaning up a probe handle with CURLOPT_HSTS set to that path can
         * publish an empty cache over committed HSTS state.
         */
        REQUIRE_OPTION(setup_error, probe, CURLOPT_HSTS, NULL, "HSTS cache configuration", "--hsts-file");
#else
        return capability_failure(setup_error, "HSTS support in the build headers", "--hsts-file", CURLE_NOT_BUILT_IN);
#endif
    }

    if (configured_string(cfg->https.pinnedpubkey)) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_PINNEDPUBLICKEY, cfg->https.pinnedpubkey, "public-key pinning", "--pinnedpubkey");
    }

    if (configured_string(cfg->download.dns_servers)) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_DNS_SERVERS, cfg->download.dns_servers, "custom DNS servers", "--dns-servers");
    }
    if (configured_string(cfg->download.bind_dns_address)) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_DNS_LOCAL_IP4, cfg->download.bind_dns_address, "DNS resolver address binding", "--bind-dns-address");
    }

    bool require_ipv4 = cfg->download.inet4_only || (configured_string(cfg->download.prefer_family) && strcasecmp(cfg->download.prefer_family, "IPv4") == 0);
    if (require_ipv4) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4, "IPv4 address-family control", "--inet4-only/--prefer-family");
    }

    bool require_ipv6 = cfg->download.inet6_only || (configured_string(cfg->download.prefer_family) && strcasecmp(cfg->download.prefer_family, "IPv6") == 0);
    if (require_ipv6) {
        if (require_feature(setup_error, version, CURL_VERSION_IPV6, "IPv6 support", "--inet6-only/--prefer-family") != 0) {
            return -1;
        }
        REQUIRE_OPTION(setup_error, probe, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6, "IPv6 address-family control", "--inet6-only/--prefer-family");
    }

#if BX_FETCH_HTTP2_ENABLED
    if (require_feature(setup_error, version, CURL_VERSION_HTTP2, "HTTP/2 support", "http2 build option") != 0) {
        return -1;
    }
    REQUIRE_OPTION(setup_error, probe, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS, "HTTP/2 selection", "http2 build option");
#else
    REQUIRE_OPTION(setup_error, probe, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1, "HTTP/1.1 selection", "http2 build option");
#endif

    if (cfg->download.no_proxy) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_NOPROXY, "*", "proxy bypass", "--proxy=off/--no-proxy");
    }
    BxFetchCredentialSelection proxy_credentials;
    bx_fetch_net_select_proxy_credentials(cfg, &proxy_credentials);
    if (proxy_credentials.source == BX_FETCH_CREDENTIAL_SOURCE_PROXY_CONFIG) {
        REQUIRE_OPTION(setup_error, probe, CURLOPT_PROXYUSERNAME, proxy_credentials.username, "proxy username configuration", "--proxy-user");
        REQUIRE_OPTION(setup_error, probe, CURLOPT_PROXYPASSWORD, proxy_credentials.password, "proxy password configuration", "--proxy-password");
    }
    if (environment_requests_https_proxy(cfg, requests_http, requests_https, requests_ftp || requests_ftps) &&
        require_feature(setup_error, version, CURL_VERSION_HTTPS_PROXY, "HTTPS-proxy support", "HTTPS proxy environment") != 0) {
        return -1;
    }

    bool requests_ftp_behavior = requests_ftp || requests_ftps || configured_string(cfg->ftp.ftp_user) || configured_string(cfg->ftp.ftp_password) || cfg->ftp.no_passive_ftp;
    if (requests_ftp_behavior && !protocol_available(version, "ftp") && !protocol_available(version, "ftps")) {
        return capability_failure(setup_error, "FTP or FTPS protocol support", "FTP configuration", CURLE_UNSUPPORTED_PROTOCOL);
    }
    if (requests_ftps && require_feature(setup_error, version, CURL_VERSION_SSL, "TLS support required by FTPS", "FTPS request URL") != 0) {
        return -1;
    }
    if (requests_ftp_behavior) {
        if (cfg->ftp.no_passive_ftp) {
            REQUIRE_OPTION(setup_error, probe, CURLOPT_FTPPORT, "-", "active FTP mode", "--no-passive-ftp");
        }
        else {
            REQUIRE_OPTION(setup_error, probe, CURLOPT_FTP_USE_EPSV, 1L, "passive FTP mode", "FTP passive mode");
        }
    }

    return 0;
}

#undef REQUIRE_OPTION
