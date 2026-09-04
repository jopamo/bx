#define _GNU_SOURCE
#include "lib/fetch/url.h"
#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char* scheme;
    BxFetchProtocol protocol;
} BxFetchProtocolDefinition;

static const BxFetchProtocolDefinition k_supported_protocols[] = {
    {"http", BX_FETCH_PROTOCOL_HTTP},
    {"https", BX_FETCH_PROTOCOL_HTTPS},
    {"ftp", BX_FETCH_PROTOCOL_FTP},
    {"ftps", BX_FETCH_PROTOCOL_FTPS},
};

static BxFetchProtocol protocol_from_scheme_span(const char* scheme, size_t scheme_len) {
    if (!scheme || scheme_len == 0)
        return BX_FETCH_PROTOCOL_NONE;

    for (size_t i = 0; i < sizeof(k_supported_protocols) / sizeof(k_supported_protocols[0]); i++) {
        if (strlen(k_supported_protocols[i].scheme) == scheme_len && strncasecmp(scheme, k_supported_protocols[i].scheme, scheme_len) == 0) {
            return k_supported_protocols[i].protocol;
        }
    }

    return BX_FETCH_PROTOCOL_NONE;
}

static bool url_scheme_span(const char* url, const char** scheme, size_t* scheme_len) {
    if (!url || !scheme || !scheme_len || !isalpha((unsigned char)url[0])) {
        return false;
    }

    const char* p = url + 1;
    while (*p && (isalnum((unsigned char)*p) || *p == '+' || *p == '-' || *p == '.')) {
        p++;
    }
    if (*p != ':')
        return false;

    *scheme = url;
    *scheme_len = (size_t)(p - url);
    return true;
}

bool bx_fetch_url_has_scheme(const char* url, const char* expected_scheme) {
    if (!url || !expected_scheme)
        return false;

    const char* scheme = NULL;
    size_t scheme_len = 0;
    if (!url_scheme_span(url, &scheme, &scheme_len) || strlen(expected_scheme) != scheme_len || strncasecmp(scheme, expected_scheme, scheme_len) != 0) {
        return false;
    }

    const char* authority = scheme + scheme_len + 1;
    return authority[0] == '/' && authority[1] == '/';
}

bool bx_fetch_url_has_userinfo(const char* url) {
    const char* scheme = NULL;
    size_t scheme_len = 0;
    if (!url_scheme_span(url, &scheme, &scheme_len))
        return false;

    const char* authority = scheme + scheme_len + 1;
    if (authority[0] != '/' || authority[1] != '/')
        return false;
    authority += 2;

    size_t authority_len = strcspn(authority, "/?#");
    return memchr(authority, '@', authority_len) != NULL;
}

static bool url_has_explicit_authority(const char* url) {
    const char* scheme = NULL;
    size_t scheme_len = 0;
    if (!url_scheme_span(url, &scheme, &scheme_len))
        return false;

    const char* authority = scheme + scheme_len + 1;
    if (authority[0] != '/' || authority[1] != '/')
        return false;
    authority += 2;
    return authority[0] != '\0' && authority[0] != '/' && authority[0] != '?' && authority[0] != '#';
}

static void ascii_lowercase(char* s) {
    if (!s)
        return;
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

typedef enum {
    URL_COMPONENT_USERINFO = 0,
    URL_COMPONENT_PATH,
    URL_COMPONENT_QUERY,
    URL_COMPONENT_ZONE_ID,
} URLComponentKind;

static bool ascii_is_hex(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static unsigned char ascii_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return (unsigned char)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (unsigned char)(10 + c - 'a');
    return (unsigned char)(10 + c - 'A');
}

static char ascii_hex_digit(unsigned char value) {
    return value < 10 ? (char)('0' + value) : (char)('A' + value - 10);
}

static bool url_has_valid_percent_escapes(const char* url) {
    if (!url)
        return false;

    for (size_t i = 0; url[i] != '\0'; i++) {
        if (url[i] != '%')
            continue;
        if (url[i + 1] == '\0' || url[i + 2] == '\0')
            return false;
        if (!ascii_is_hex((unsigned char)url[i + 1]) || !ascii_is_hex((unsigned char)url[i + 2])) {
            return false;
        }
        i += 2;
    }
    return true;
}

static bool uri_is_unreserved(unsigned char c) {
    return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static bool uri_is_subdelimiter(unsigned char c) {
    switch (c) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
            return true;
        default:
            return false;
    }
}

static bool uri_component_allows(URLComponentKind kind, unsigned char c) {
    if (uri_is_unreserved(c) || uri_is_subdelimiter(c))
        return true;
    if (kind == URL_COMPONENT_USERINFO)
        return c == ':';
    if (kind == URL_COMPONENT_PATH)
        return c == ':' || c == '@' || c == '/';
    if (kind == URL_COMPONENT_QUERY) {
        return c == ':' || c == '@' || c == '/' || c == '?';
    }
    return false;
}

static char* canonicalize_uri_component(const char* value, URLComponentKind kind) {
    if (!value)
        return NULL;

    size_t len = strlen(value);
    if (len > (SIZE_MAX - 1U) / 3U) {
        errno = ENOMEM;
        return NULL;
    }

    char* canonical = malloc((len * 3U) + 1U);
    if (!canonical)
        return NULL;

    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c == '%') {
            if (i + 2 >= len || !ascii_is_hex((unsigned char)value[i + 1]) || !ascii_is_hex((unsigned char)value[i + 2])) {
                free(canonical);
                errno = EINVAL;
                return NULL;
            }

            unsigned char decoded = (unsigned char)((ascii_hex_value((unsigned char)value[i + 1]) << 4U) | ascii_hex_value((unsigned char)value[i + 2]));
            canonical[out++] = '%';
            canonical[out++] = ascii_hex_digit((unsigned char)(decoded >> 4U));
            canonical[out++] = ascii_hex_digit((unsigned char)(decoded & 0x0FU));
            i += 2;
        }
        else if (c < 0x80U && uri_component_allows(kind, c)) {
            canonical[out++] = (char)c;
        }
        else {
            canonical[out++] = '%';
            canonical[out++] = ascii_hex_digit((unsigned char)(c >> 4U));
            canonical[out++] = ascii_hex_digit((unsigned char)(c & 0x0FU));
        }
    }

    canonical[out] = '\0';
    return canonical;
}

static bool string_is_ascii(const char* value) {
    if (!value)
        return false;
    for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; p++) {
        if (*p >= 0x80U)
            return false;
    }
    return true;
}

BxFetchProtocol bx_fetch_protocol_from_scheme(const char* scheme) {
    if (!scheme || scheme[0] == '\0')
        return BX_FETCH_PROTOCOL_NONE;
    return protocol_from_scheme_span(scheme, strlen(scheme));
}

unsigned int bx_fetch_protocol_policy_mask(bool https_only) {
    if (https_only)
        return BX_FETCH_PROTOCOL_HTTPS;

    unsigned int mask = BX_FETCH_PROTOCOL_NONE;
    for (size_t i = 0; i < sizeof(k_supported_protocols) / sizeof(k_supported_protocols[0]); i++) {
        mask |= (unsigned int)k_supported_protocols[i].protocol;
    }
    return mask;
}

BxFetchProtocolDecision bx_fetch_protocol_policy_evaluate_scheme(const char* scheme, bool https_only) {
    BxFetchProtocol protocol = bx_fetch_protocol_from_scheme(scheme);
    if (protocol == BX_FETCH_PROTOCOL_NONE) {
        return BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED;
    }
    if (((unsigned int)protocol & bx_fetch_protocol_policy_mask(https_only)) == 0) {
        return BX_FETCH_PROTOCOL_DECISION_HTTPS_ONLY;
    }
    return BX_FETCH_PROTOCOL_DECISION_ALLOW;
}

BxFetchProtocolDecision bx_fetch_protocol_policy_evaluate_url(const char* url, bool https_only) {
    const char* scheme = NULL;
    size_t scheme_len = 0;
    if (!url_scheme_span(url, &scheme, &scheme_len)) {
        return BX_FETCH_PROTOCOL_DECISION_INVALID_URL;
    }
    BxFetchProtocol protocol = protocol_from_scheme_span(scheme, scheme_len);
    if (protocol == BX_FETCH_PROTOCOL_NONE) {
        return BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED;
    }

    BxFetchUrl* parsed = bx_fetch_url_parse(url);
    if (!parsed)
        return BX_FETCH_PROTOCOL_DECISION_INVALID_URL;

    BxFetchProtocolDecision decision = bx_fetch_protocol_policy_evaluate_scheme(parsed->scheme, https_only);
    bx_fetch_url_free(parsed);
    return decision;
}

const char* bx_fetch_protocol_decision_reason(BxFetchProtocolDecision decision) {
    switch (decision) {
        case BX_FETCH_PROTOCOL_DECISION_ALLOW:
            return NULL;
        case BX_FETCH_PROTOCOL_DECISION_INVALID_URL:
            return "invalid-url";
        case BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED:
            return "unsupported-protocol";
        case BX_FETCH_PROTOCOL_DECISION_HTTPS_ONLY:
            return "https-only";
    }

    return "unsupported-protocol";
}

bool bx_fetch_protocol_policy_format(bool https_only, char* out, size_t out_size) {
    if (!out || out_size == 0)
        return false;

    unsigned int allowed = bx_fetch_protocol_policy_mask(https_only);
    size_t written = 0;
    out[0] = '\0';

    for (size_t i = 0; i < sizeof(k_supported_protocols) / sizeof(k_supported_protocols[0]); i++) {
        if ((allowed & (unsigned int)k_supported_protocols[i].protocol) == 0) {
            continue;
        }

        size_t scheme_len = strlen(k_supported_protocols[i].scheme);
        size_t separator_len = written > 0 ? 1 : 0;
        if (scheme_len + separator_len >= out_size - written) {
            out[0] = '\0';
            return false;
        }
        if (separator_len > 0) {
            out[written++] = ',';
        }
        memcpy(out + written, k_supported_protocols[i].scheme, scheme_len);
        written += scheme_len;
        out[written] = '\0';
    }

    return written > 0;
}

static bool is_default_port(const char* scheme, const char* port) {
    if (!scheme || !port || port[0] == '\0')
        return false;
    if (strcasecmp(scheme, "http") == 0)
        return strcmp(port, "80") == 0;
    if (strcasecmp(scheme, "https") == 0)
        return strcmp(port, "443") == 0;
    if (strcasecmp(scheme, "ftp") == 0)
        return strcmp(port, "21") == 0;
    if (strcasecmp(scheme, "ftps") == 0)
        return strcmp(port, "990") == 0;
    return false;
}

BxFetchUrl* bx_fetch_url_parse(const char* url) {
    CURLU* h = curl_url();
    if (!h)
        return NULL;

    if (curl_url_set(h, CURLUPART_URL, url, 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return NULL;
    }

    BxFetchUrl* mu = calloc(1, sizeof(BxFetchUrl));
    if (!mu) {
        curl_url_cleanup(h);
        return NULL;
    }

    curl_url_get(h, CURLUPART_SCHEME, &mu->scheme, 0);
    curl_url_get(h, CURLUPART_USER, &mu->user, 0);
    curl_url_get(h, CURLUPART_PASSWORD, &mu->password, 0);
    curl_url_get(h, CURLUPART_HOST, &mu->host, 0);

    char* port_str = NULL;
    if (curl_url_get(h, CURLUPART_PORT, &port_str, 0) == CURLUE_OK) {
        mu->port = atoi(port_str);
        curl_free(port_str);
    }

    curl_url_get(h, CURLUPART_PATH, &mu->path, 0);
    curl_url_get(h, CURLUPART_QUERY, &mu->query, 0);
    curl_url_get(h, CURLUPART_FRAGMENT, &mu->fragment, 0);

    // Note: libcurl allocates these strings, we must free them with curl_free or strdup them
    // Actually, curl_url_get gives us strings that we should free with curl_free.
    // To be safe and independent of curl_free in our own free function, we'll strdup them.

    char* s;
    if ((s = mu->scheme)) {
        mu->scheme = strdup(s);
        curl_free(s);
    }
    if ((s = mu->user)) {
        mu->user = strdup(s);
        curl_free(s);
    }
    if ((s = mu->password)) {
        mu->password = strdup(s);
        curl_free(s);
    }
    if ((s = mu->host)) {
        mu->host = strdup(s);
        curl_free(s);
    }
    if ((s = mu->path)) {
        mu->path = strdup(s);
        curl_free(s);
    }
    if ((s = mu->query)) {
        mu->query = strdup(s);
        curl_free(s);
    }
    if ((s = mu->fragment)) {
        mu->fragment = strdup(s);
        curl_free(s);
    }

    curl_url_cleanup(h);
    return mu;
}

void bx_fetch_url_free(BxFetchUrl* mu) {
    if (!mu)
        return;
    free(mu->scheme);
    free(mu->user);
    free(mu->password);
    free(mu->host);
    free(mu->path);
    free(mu->query);
    free(mu->fragment);
    free(mu);
}

char* bx_fetch_url_resolve(const char* base_url, const char* relative_url) {
    CURLU* h = curl_url();
    if (!h)
        return NULL;

    if (curl_url_set(h, CURLUPART_URL, base_url, 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return NULL;
    }

    if (curl_url_set(h, CURLUPART_URL, relative_url, 0) != CURLUE_OK) {
        curl_url_cleanup(h);
        return NULL;
    }

    char* resolved = NULL;
    if (curl_url_get(h, CURLUPART_URL, &resolved, 0) == CURLUE_OK) {
        char* ret = strdup(resolved);
        curl_free(resolved);
        curl_url_cleanup(h);
        return ret;
    }

    curl_url_cleanup(h);
    return NULL;
}

char* bx_fetch_url_resolve_canonical(const char* base_url, const char* relative_url) {
    char* resolved = bx_fetch_url_resolve(base_url, relative_url);
    if (!resolved)
        return NULL;

    char* canonical = bx_fetch_url_canonicalize(resolved);
    free(resolved);
    return canonical;
}

char* bx_fetch_url_to_string(BxFetchUrl* mu) {
    CURLU* h = curl_url();
    if (!h)
        return NULL;

    if (mu->scheme)
        curl_url_set(h, CURLUPART_SCHEME, mu->scheme, 0);
    if (mu->user)
        curl_url_set(h, CURLUPART_USER, mu->user, 0);
    if (mu->password)
        curl_url_set(h, CURLUPART_PASSWORD, mu->password, 0);
    if (mu->host)
        curl_url_set(h, CURLUPART_HOST, mu->host, 0);
    if (mu->port > 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", mu->port);
        curl_url_set(h, CURLUPART_PORT, port_str, 0);
    }
    if (mu->path)
        curl_url_set(h, CURLUPART_PATH, mu->path, 0);
    if (mu->query)
        curl_url_set(h, CURLUPART_QUERY, mu->query, 0);
    if (mu->fragment)
        curl_url_set(h, CURLUPART_FRAGMENT, mu->fragment, 0);

    char* url = NULL;
    if (curl_url_get(h, CURLUPART_URL, &url, 0) == CURLUE_OK) {
        char* ret = strdup(url);
        curl_free(url);
        curl_url_cleanup(h);
        return ret;
    }

    curl_url_cleanup(h);
    return NULL;
}

static char* canonicalize_url(const char* url, bool include_userinfo) {
    errno = 0;
    if (!url || !url_has_explicit_authority(url) || !url_has_valid_percent_escapes(url)) {
        errno = EINVAL;
        return NULL;
    }

    char* ret = NULL;
    CURLU* src = curl_url();
    if (!src) {
        errno = ENOMEM;
        return NULL;
    }
    if (curl_url_set(src, CURLUPART_URL, url, 0) != CURLUE_OK) {
        curl_url_cleanup(src);
        errno = EINVAL;
        return NULL;
    }

    CURLU* dst = curl_url();
    if (!dst) {
        curl_url_cleanup(src);
        errno = ENOMEM;
        return NULL;
    }

    char* scheme = NULL;
    char* host = NULL;
    char* user = NULL;
    char* password = NULL;
    char* port = NULL;
    char* path = NULL;
    char* query = NULL;
    char* zone_id = NULL;
    char* canonical_user = NULL;
    char* canonical_password = NULL;
    char* canonical_path = NULL;
    char* canonical_query = NULL;
    char* canonical_zone_id = NULL;

    if (curl_url_get(src, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK && scheme) {
        ascii_lowercase(scheme);
        if (curl_url_set(dst, CURLUPART_SCHEME, scheme, 0) != CURLUE_OK)
            goto cleanup;
    }
    else {
        errno = EINVAL;
        goto cleanup;
    }
#ifdef CURLU_PUNYCODE
    CURLUcode host_rc = curl_url_get(src, CURLUPART_HOST, &host, CURLU_PUNYCODE);
#else
    CURLUcode host_rc = curl_url_get(src, CURLUPART_HOST, &host, 0);
#endif
    if (host_rc == CURLUE_OK && host && host[0] != '\0' && string_is_ascii(host)) {
        ascii_lowercase(host);
        if (curl_url_set(dst, CURLUPART_HOST, host, 0) != CURLUE_OK)
            goto cleanup;
    }
    else {
        errno = (host_rc == CURLUE_LACKS_IDN) ? ENOTSUP : EINVAL;
        goto cleanup;
    }
    if (include_userinfo && curl_url_get(src, CURLUPART_USER, &user, 0) == CURLUE_OK && user) {
        canonical_user = canonicalize_uri_component(user, URL_COMPONENT_USERINFO);
        if (!canonical_user || curl_url_set(dst, CURLUPART_USER, canonical_user, 0) != CURLUE_OK) {
            goto cleanup;
        }
    }
    if (include_userinfo && curl_url_get(src, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK && password) {
        canonical_password = canonicalize_uri_component(password, URL_COMPONENT_USERINFO);
        if (!canonical_password || curl_url_set(dst, CURLUPART_PASSWORD, canonical_password, 0) != CURLUE_OK) {
            goto cleanup;
        }
    }
    if (curl_url_get(src, CURLUPART_PATH, &path, 0) == CURLUE_OK && path && path[0] != '\0') {
        canonical_path = canonicalize_uri_component(path, URL_COMPONENT_PATH);
        if (!canonical_path || curl_url_set(dst, CURLUPART_PATH, canonical_path, 0) != CURLUE_OK) {
            goto cleanup;
        }
    }
    else if (curl_url_set(dst, CURLUPART_PATH, "/", 0) != CURLUE_OK) {
        goto cleanup;
    }
    CURLUcode query_rc = curl_url_get(src, CURLUPART_QUERY, &query, CURLU_GET_EMPTY);
    if (query_rc == CURLUE_OK && query) {
        canonical_query = canonicalize_uri_component(query, URL_COMPONENT_QUERY);
        if (!canonical_query || curl_url_set(dst, CURLUPART_QUERY, canonical_query, 0) != CURLUE_OK) {
            goto cleanup;
        }
    }
    if (curl_url_get(src, CURLUPART_PORT, &port, 0) == CURLUE_OK && port && port[0] != '\0') {
        if (!is_default_port(scheme, port)) {
            if (curl_url_set(dst, CURLUPART_PORT, port, 0) != CURLUE_OK)
                goto cleanup;
        }
    }
    if (curl_url_get(src, CURLUPART_ZONEID, &zone_id, 0) == CURLUE_OK && zone_id) {
        canonical_zone_id = canonicalize_uri_component(zone_id, URL_COMPONENT_ZONE_ID);
        if (!canonical_zone_id || curl_url_set(dst, CURLUPART_ZONEID, canonical_zone_id, 0) != CURLUE_OK) {
            goto cleanup;
        }
    }

    char* out = NULL;
    if (curl_url_get(dst, CURLUPART_URL, &out, CURLU_GET_EMPTY) != CURLUE_OK || !out) {
        goto cleanup;
    }

    ret = strdup(out);
    curl_free(out);

cleanup:
    if (scheme)
        curl_free(scheme);
    if (host)
        curl_free(host);
    if (user)
        curl_free(user);
    if (password)
        curl_free(password);
    if (port)
        curl_free(port);
    if (path)
        curl_free(path);
    if (query)
        curl_free(query);
    if (zone_id)
        curl_free(zone_id);
    free(canonical_user);
    free(canonical_password);
    free(canonical_path);
    free(canonical_query);
    free(canonical_zone_id);
    curl_url_cleanup(dst);
    curl_url_cleanup(src);
    if (!ret && errno == 0)
        errno = EINVAL;
    return ret;
}

char* bx_fetch_url_canonicalize(const char* url) {
    return canonicalize_url(url, true);
}

char* bx_fetch_url_display_safe(const char* url) {
    char* canonical = canonicalize_url(url, false);
    if (!canonical) {
        const char* scheme = NULL;
        size_t scheme_len = 0;
        if (!url_scheme_span(url, &scheme, &scheme_len) || protocol_from_scheme_span(scheme, scheme_len) != BX_FETCH_PROTOCOL_NONE || !url_has_explicit_authority(url) ||
            !url_has_valid_percent_escapes(url)) {
            return NULL;
        }

#ifdef CURLU_NON_SUPPORT_SCHEME
        /*
         * Unsupported schemes are rejected before request canonicalization,
         * but their diagnostics still need safe text. Let CURLU identify and
         * remove authority userinfo without making that scheme transferable.
         */
        CURLU* unsupported = curl_url();
        if (!unsupported) {
            errno = ENOMEM;
            return NULL;
        }
        char* display = NULL;
        if (curl_url_set(unsupported, CURLUPART_URL, url, CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK && curl_url_set(unsupported, CURLUPART_USER, NULL, 0) == CURLUE_OK &&
            curl_url_set(unsupported, CURLUPART_PASSWORD, NULL, 0) == CURLUE_OK && curl_url_set(unsupported, CURLUPART_FRAGMENT, NULL, 0) == CURLUE_OK &&
            curl_url_get(unsupported, CURLUPART_URL, &display, CURLU_GET_EMPTY) == CURLUE_OK && display) {
            canonical = strdup(display);
            curl_free(display);
        }
        curl_url_cleanup(unsupported);
        if (!canonical) {
            if (errno == 0)
                errno = EINVAL;
            return NULL;
        }
#else
        return NULL;
#endif
    }
    return canonical;
}
