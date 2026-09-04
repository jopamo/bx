#include "credentials.h"
#include "lib/fetch/url.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool pair_is_configured(const char* username, const char* password) {
    return username != NULL || password != NULL;
}

static void select_configured_pair(MiraCredentialSelection* selection, MiraCredentialSource source, const char* username, const char* password) {
    selection->source = source;
    selection->username = username ? username : "";
    selection->password = password ? password : "";
}

void bx_fetch_net_select_origin_credentials(const EffectiveConfig* cfg, const char* url, MiraCredentialSelection* selection) {
    if (!selection)
        return;
    *selection = (MiraCredentialSelection){0};
    if (!cfg || !url)
        return;

    bool is_http = bx_fetch_url_has_scheme(url, "http") || bx_fetch_url_has_scheme(url, "https");
    bool is_ftp = bx_fetch_url_has_scheme(url, "ftp") || bx_fetch_url_has_scheme(url, "ftps");

    if (is_http && pair_is_configured(cfg->http.http_user, cfg->http.http_password)) {
        select_configured_pair(selection, BX_FETCH_CREDENTIAL_SOURCE_HTTP, cfg->http.http_user, cfg->http.http_password);
        return;
    }
    if (is_ftp && pair_is_configured(cfg->ftp.ftp_user, cfg->ftp.ftp_password)) {
        select_configured_pair(selection, BX_FETCH_CREDENTIAL_SOURCE_FTP, cfg->ftp.ftp_user, cfg->ftp.ftp_password);
        return;
    }
    if (pair_is_configured(cfg->download.user, cfg->download.password)) {
        select_configured_pair(selection, BX_FETCH_CREDENTIAL_SOURCE_GENERIC, cfg->download.user, cfg->download.password);
        return;
    }
    if (bx_fetch_url_has_userinfo(url)) {
        selection->source = BX_FETCH_CREDENTIAL_SOURCE_URL;
    }
}

void bx_fetch_net_select_proxy_credentials(const EffectiveConfig* cfg, MiraCredentialSelection* selection) {
    if (!selection)
        return;
    *selection = (MiraCredentialSelection){
        .source = BX_FETCH_CREDENTIAL_SOURCE_PROXY_INHERITED,
    };
    if (!cfg)
        return;

    if (pair_is_configured(cfg->http.proxy_user, cfg->http.proxy_password)) {
        select_configured_pair(selection, BX_FETCH_CREDENTIAL_SOURCE_PROXY_CONFIG, cfg->http.proxy_user, cfg->http.proxy_password);
    }
}

static const char* first_environment_value(const char* lower_name, const char* upper_name) {
    const char* value = lower_name ? getenv(lower_name) : NULL;
    if (value && value[0] != '\0')
        return value;
    value = upper_name ? getenv(upper_name) : NULL;
    return value && value[0] != '\0' ? value : NULL;
}

const char* bx_fetch_net_proxy_environment_url(const char* request_url) {
    const char* proxy_url = NULL;

    if (bx_fetch_url_has_scheme(request_url, "http")) {
        /* Uppercase HTTP_PROXY is intentionally ignored by libcurl. */
        proxy_url = first_environment_value("http_proxy", NULL);
    }
    else if (bx_fetch_url_has_scheme(request_url, "https")) {
        proxy_url = first_environment_value("https_proxy", "HTTPS_PROXY");
    }
    else if (bx_fetch_url_has_scheme(request_url, "ftp")) {
        proxy_url = first_environment_value("ftp_proxy", "FTP_PROXY");
    }
    else if (bx_fetch_url_has_scheme(request_url, "ftps")) {
        proxy_url = first_environment_value("ftps_proxy", "FTPS_PROXY");
    }

    if (proxy_url)
        return proxy_url;
    return first_environment_value("all_proxy", "ALL_PROXY");
}

static bool proxy_authority_has_userinfo(const char* proxy_url) {
    if (!proxy_url)
        return false;
    const char* scheme_separator = strstr(proxy_url, "://");
    if (scheme_separator) {
        return bx_fetch_url_has_userinfo(proxy_url);
    }

    size_t authority_len = strcspn(proxy_url, "/?#");
    return memchr(proxy_url, '@', authority_len) != NULL;
}

int bx_fetch_net_sanitize_proxy_url_for_explicit_credentials(const char* proxy_url, char** sanitized_out) {
    if (!sanitized_out)
        return -1;
    *sanitized_out = NULL;
    if (!proxy_authority_has_userinfo(proxy_url))
        return 0;

    const char* parse_url = proxy_url;
    char* owned_parse_url = NULL;
    if (!strstr(proxy_url, "://")) {
        static const char prefix[] = "http://";
        size_t proxy_len = strlen(proxy_url);
        if (proxy_len > SIZE_MAX - sizeof(prefix))
            return -1;
        owned_parse_url = malloc(sizeof(prefix) - 1 + proxy_len + 1);
        if (!owned_parse_url)
            return -1;
        memcpy(owned_parse_url, prefix, sizeof(prefix) - 1);
        memcpy(owned_parse_url + sizeof(prefix) - 1, proxy_url, proxy_len + 1);
        parse_url = owned_parse_url;
    }

    char* sanitized = bx_fetch_url_display_safe(parse_url);
    free(owned_parse_url);
    if (!sanitized)
        return -1;

    *sanitized_out = sanitized;
    return 0;
}
