#define _GNU_SOURCE
#include "engine_internal.h"
#include "credentials.h"
#include "lib/fetch/http_header.h"
#include "lib/fetch/url.h"
#include <curl/curl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef BX_FETCH_HTTP2_ENABLED
#define BX_FETCH_HTTP2_ENABLED 0
#endif

static long effective_connect_phase_timeout_seconds(const struct bx_fetch_config* cfg) {
    if (!cfg)
        return 0L;

    long timeout = 0L;
    if (cfg->download.dns_timeout > 0) {
        timeout = (long)cfg->download.dns_timeout;
    }
    if (cfg->download.connect_timeout > 0) {
        long connect_timeout = (long)cfg->download.connect_timeout;
        if (timeout <= 0 || connect_timeout < timeout) {
            timeout = connect_timeout;
        }
    }

    return timeout;
}

static int record_setopt_result(BxFetchNetSetupError* setup_error, const char* option_name, CURLcode result) {
    if (result == CURLE_OK)
        return 0;

    if (setup_error) {
        setup_error->present = true;
        setup_error->option_name = option_name;
        setup_error->detail = NULL;
        setup_error->curl_code = (int)result;
    }
    return -1;
}

#define SETOPT_OR_RETURN(curl, setup_error, option, value)                      \
    do {                                                                        \
        CURLcode setopt_result = curl_easy_setopt((curl), (option), (value));   \
        if (record_setopt_result((setup_error), #option, setopt_result) != 0) { \
            return -1;                                                          \
        }                                                                       \
    } while (0)

static int record_header_setup_error(BxFetchNetSetupError* setup_error, BxFetchHttpHeaderError error) {
    if (setup_error) {
        setup_error->present = true;
        setup_error->option_name = NULL;
        setup_error->detail = bx_fetch_http_header_error_string(error);
        setup_error->curl_code = -1;
    }
    return -1;
}

static int append_prepared_header(BxFetchTransfer* t, const char* header_line, BxFetchNetSetupError* setup_error) {
    if (!t || !header_line) {
        return record_header_setup_error(setup_error, BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT);
    }

    struct curl_slist* grown = curl_slist_append(t->headers, header_line);
    if (!grown) {
        return record_header_setup_error(setup_error, BX_FETCH_HTTP_HEADER_OUT_OF_MEMORY);
    }
    t->headers = grown;
    return 0;
}

static size_t bx_fetch_request_body_read_callback(char* buffer, size_t size, size_t nmemb, void* userdata) {
    BxFetchTransfer* t = userdata;
    if (!t || !t->req || !bx_fetch_request_has_body_file(t->req)) {
        errno = EINVAL;
        if (t)
            t->request_body_io_failed = true;
        bx_fetch_transfer_mark_io_failure(t, EINVAL);
        return CURL_READFUNC_ABORT;
    }
    if (size != 0 && nmemb > SIZE_MAX / size) {
        errno = EOVERFLOW;
        t->request_body_io_failed = true;
        bx_fetch_transfer_mark_io_failure(t, EOVERFLOW);
        return CURL_READFUNC_ABORT;
    }

    size_t nread = 0;
    if (bx_fetch_request_body_file_read(t->req, buffer, size * nmemb, &nread) != 0) {
        t->request_body_io_failed = true;
        bx_fetch_transfer_mark_io_failure(t, EIO);
        return CURL_READFUNC_ABORT;
    }
    return nread;
}

static int bx_fetch_request_body_seek_callback(void* userdata, curl_off_t offset, int origin) {
    BxFetchTransfer* t = userdata;
    int64_t request_offset = (int64_t)offset;
    if (!t || !t->req || (curl_off_t)request_offset != offset || bx_fetch_request_body_file_seek(t->req, request_offset, origin) != 0) {
        if (t)
            t->request_body_io_failed = true;
        bx_fetch_transfer_mark_io_failure(t, EIO);
        return CURL_SEEKFUNC_FAIL;
    }
    return CURL_SEEKFUNC_OK;
}

static int prepare_bx_fetch_request_headers(BxFetchEngine* engine, BxFetchTransfer* t, BxFetchNetSetupError* setup_error) {
    if (!engine || !engine->cfg || !t || !t->req || engine->cfg->http.header_count < 0 || (engine->cfg->http.header_count > 0 && !engine->cfg->http.headers) ||
        (t->req->header_count > 0 && !t->req->headers)) {
        return record_header_setup_error(setup_error, BX_FETCH_HTTP_HEADER_INVALID_ARGUMENT);
    }

    for (int i = 0; i < engine->cfg->http.header_count; i++) {
        char* formatted = NULL;
        BxFetchHttpHeaderError error = bx_fetch_http_header_format_line_for_curl(engine->cfg->http.headers[i], &formatted);
        if (error != BX_FETCH_HTTP_HEADER_OK) {
            free(formatted);
            return record_header_setup_error(setup_error, error);
        }
        int append_result = append_prepared_header(t, formatted, setup_error);
        free(formatted);
        if (append_result != 0)
            return -1;
    }

    for (size_t i = 0; i < t->req->header_count; i++) {
        char* formatted = NULL;
        BxFetchHttpHeaderError error = bx_fetch_http_header_format_pair_for_curl(t->req->headers[i].name, t->req->headers[i].value, &formatted);
        if (error != BX_FETCH_HTTP_HEADER_OK) {
            free(formatted);
            return record_header_setup_error(setup_error, error);
        }
        int append_result = append_prepared_header(t, formatted, setup_error);
        free(formatted);
        if (append_result != 0)
            return -1;
    }

    return 0;
}

static int set_protocol_restrictions(CURL* curl, bool https_only, BxFetchNetSetupError* setup_error) {
    if (!curl)
        return -1;

#if LIBCURL_VERSION_NUM >= 0x075500
    char protocols[32];
    if (!bx_fetch_protocol_policy_format(https_only, protocols, sizeof(protocols))) {
        return -1;
    }
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PROTOCOLS_STR, protocols);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
#else
    unsigned int allowed = bx_fetch_protocol_policy_mask(https_only);
    long curl_protocols = 0;
    if ((allowed & BX_FETCH_PROTOCOL_HTTP) != 0)
        curl_protocols |= CURLPROTO_HTTP;
    if ((allowed & BX_FETCH_PROTOCOL_HTTPS) != 0)
        curl_protocols |= CURLPROTO_HTTPS;
    if ((allowed & BX_FETCH_PROTOCOL_FTP) != 0)
        curl_protocols |= CURLPROTO_FTP;
    if ((allowed & BX_FETCH_PROTOCOL_FTPS) != 0)
        curl_protocols |= CURLPROTO_FTPS;
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PROTOCOLS, curl_protocols);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_REDIR_PROTOCOLS, curl_protocols);
#endif

    return 0;
}

static int set_credentials(CURL* curl, BxFetchNetSetupError* setup_error, const char* username, const char* password) {
    if (!curl || !username || !password)
        return -1;

    /*
     * Keep username and password separate. Libcurl copies both strings during
     * setopt, so their full lengths are preserved without temporary storage.
     * This also avoids CURLOPT_USERPWD's colon ambiguity in usernames.
     */
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_USERNAME, username);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PASSWORD, password);
    return 0;
}

static int set_proxy_credentials(CURL* curl, BxFetchNetSetupError* setup_error, const char* proxy_url, const char* username, const char* password) {
    if (!curl || !username || !password)
        return -1;

    if (proxy_url) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PROXY, proxy_url);
    }
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PROXYUSERNAME, username);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PROXYPASSWORD, password);
    return 0;
}

static int setup_easy_handle(BxFetchEngine* engine, BxFetchTransfer* t, BxFetchNetSetupError* setup_error) {
    CURL* curl = t->easy;
    const BxFetchRequest* req = t->req;

    if (!req || !req->url)
        return -1;
    if (prepare_bx_fetch_request_headers(engine, t, setup_error) != 0) {
        return -1;
    }
    if (set_protocol_restrictions(curl, engine->cfg->https.https_only, setup_error) != 0) {
        return -1;
    }

    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_URL, req->url);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_WRITEFUNCTION, bx_fetch_write_callback);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_WRITEDATA, t);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HEADERFUNCTION, bx_fetch_header_callback);
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HEADERDATA, t);

    if (req->method && strcasecmp(req->method, "GET") != 0) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_CUSTOMREQUEST, req->method);
    }
    if (req->body && bx_fetch_request_has_body_file(req))
        return -1;
    if (bx_fetch_request_has_body_file(req)) {
        uint64_t file_size = bx_fetch_request_body_file_size(req);
        curl_off_t curl_file_size = (curl_off_t)file_size;
        if (curl_file_size < 0 || (uint64_t)curl_file_size != file_size) {
            return -1;
        }
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_POST, 1L);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_READFUNCTION, bx_fetch_request_body_read_callback);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_READDATA, t);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SEEKFUNCTION, bx_fetch_request_body_seek_callback);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SEEKDATA, t);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_POSTFIELDSIZE_LARGE, curl_file_size);
    }
    else if (req->body && req->body_len > 0) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_POSTFIELDS, req->body);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
    }

    if (engine->observer.on_progress) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_XFERINFOFUNCTION, bx_fetch_progress_callback);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_XFERINFODATA, t);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_NOPROGRESS, 0L);
    }
    else {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_NOPROGRESS, 1L);
    }

    long max_redirect = (long)engine->cfg->http.max_redirect;
    if (max_redirect > 0) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FOLLOWLOCATION, 1L);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_MAXREDIRS, max_redirect);
    }
    else {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FOLLOWLOCATION, 0L);
    }

    long postredir = 0L;
    if (engine->cfg->http.redirect_method && strcasecmp(engine->cfg->http.redirect_method, "strict") == 0) {
#ifdef CURL_REDIR_POST_301
        postredir |= CURL_REDIR_POST_301;
#endif
#ifdef CURL_REDIR_POST_302
        postredir |= CURL_REDIR_POST_302;
#endif
#ifdef CURL_REDIR_POST_303
        postredir |= CURL_REDIR_POST_303;
#endif
    }
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_POSTREDIR, postredir);

    /*
     * Redirect credentials are always scoped to the initial origin. Libcurl
     * defines an origin change as a change in scheme, host, or port.
     */
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_UNRESTRICTED_AUTH, 0L);

    if (engine->cfg->http.paranoid) {
#if LIBCURL_VERSION_NUM >= 0x073D00
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
#endif
    }

    // Set user agent if configured
    if (engine->cfg->http.user_agent) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_USERAGENT, engine->cfg->http.user_agent);
    }

    // Set referer if configured
    if (engine->cfg->http.referer) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_REFERER, engine->cfg->http.referer);
    }

    if (engine->cfg->http.no_http_keep_alive) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FORBID_REUSE, 1L);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FRESH_CONNECT, 1L);
    }

    if (t->headers) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HTTPHEADER, t->headers);
    }

    if (!engine->cfg->http.no_cookies) {
        const char* cookie_source = "";
        if (engine->cfg->http.load_cookies && engine->cfg->http.load_cookies[0] != '\0') {
            cookie_source = engine->cfg->http.load_cookies;
        }

        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIEFILE, cookie_source);
        if (engine->cfg->http.save_cookies && engine->cfg->http.save_cookies[0] != '\0') {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIEJAR, engine->cfg->http.save_cookies);
        }

        if (engine->cfg->http.load_cookies && engine->cfg->http.load_cookies[0] != '\0') {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIESESSION, engine->cfg->http.keep_session_cookies ? 0L : 1L);
        }
        else {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIESESSION, 0L);
        }
    }
    else {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIEFILE, NULL);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIEJAR, NULL);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_COOKIESESSION, 0L);
    }

    // Apply per-phase timeout controls. Avoid a total wall-clock timeout so
    // large but active transfers can run past the shorthand --timeout value.
    long connect_phase_timeout = effective_connect_phase_timeout_seconds(engine->cfg);
    if (connect_phase_timeout > 0) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_CONNECTTIMEOUT, connect_phase_timeout);
    }
    if (engine->cfg->download.read_timeout > 0) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_LOW_SPEED_LIMIT, 1L);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_LOW_SPEED_TIME, (long)engine->cfg->download.read_timeout);
    }

    // DNS options
    if (engine->cfg->download.dns_servers) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_DNS_SERVERS, engine->cfg->download.dns_servers);
    }
    if (engine->cfg->download.no_dns_cache) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_DNS_CACHE_TIMEOUT, 0L);
    }
    if (engine->cfg->download.bind_dns_address) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_DNS_LOCAL_IP4, engine->cfg->download.bind_dns_address);
    }
    if (engine->cfg->download.bind_address && engine->cfg->download.bind_address[0] != '\0') {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_INTERFACE, engine->cfg->download.bind_address);
    }

    if (engine->cfg->download.inet4_only) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    }
    else if (engine->cfg->download.inet6_only) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
    }
    else if (engine->cfg->download.prefer_family && engine->cfg->download.prefer_family[0] != '\0') {
        if (strcasecmp(engine->cfg->download.prefer_family, "IPv4") == 0) {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
        }
        else if (strcasecmp(engine->cfg->download.prefer_family, "IPv6") == 0) {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V6);
        }
    }

    // TLS options
    if (engine->cfg->https.no_check_certificate) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SSL_VERIFYPEER, 0L);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SSL_VERIFYHOST, 0L);
    }
    if (engine->cfg->https.ca_certificate) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_CAINFO, engine->cfg->https.ca_certificate);
    }
    if (engine->cfg->https.ca_directory) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_CAPATH, engine->cfg->https.ca_directory);
    }
    if (engine->cfg->https.pinnedpubkey) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PINNEDPUBLICKEY, engine->cfg->https.pinnedpubkey);
    }
    if (engine->cfg->https.certificate) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SSLCERT, engine->cfg->https.certificate);
    }
    if (engine->cfg->https.private_key) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_SSLKEY, engine->cfg->https.private_key);
    }

#if LIBCURL_VERSION_NUM >= 0x074A00
    if (!engine->cfg->https.no_hsts && engine->cfg->https.hsts_file && engine->cfg->https.hsts_file[0] != '\0') {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HSTS_CTRL, CURLHSTS_ENABLE);
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HSTS, engine->cfg->https.hsts_file);
    }
#endif

#if BX_FETCH_HTTP2_ENABLED
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#else
    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
#endif

    // Auth and Proxy options
    bool allow_plaintext_credentials = !engine->cfg->http.paranoid || bx_fetch_url_has_scheme(req->url, "https");
    BxFetchCredentialSelection origin_credentials;
    bx_fetch_net_select_origin_credentials(engine->cfg, req->url, &origin_credentials);
    if (allow_plaintext_credentials && origin_credentials.username && set_credentials(curl, setup_error, origin_credentials.username, origin_credentials.password) != 0) {
        return -1;
    }
    if (engine->cfg->http.auth_no_challenge) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
    }
    else {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_HTTPAUTH, CURLAUTH_ANY);
    }

    if (engine->cfg->download.no_proxy) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_NOPROXY, "*");
    }
    BxFetchCredentialSelection proxy_credentials;
    bx_fetch_net_select_proxy_credentials(engine->cfg, &proxy_credentials);
    if (proxy_credentials.source == BX_FETCH_CREDENTIAL_SOURCE_PROXY_CONFIG) {
        char* sanitized_proxy_url = NULL;
        const char* environment_proxy_url = bx_fetch_net_proxy_environment_url(req->url);
        errno = 0;
        if (bx_fetch_net_sanitize_proxy_url_for_explicit_credentials(environment_proxy_url, &sanitized_proxy_url) != 0) {
            CURLcode code = errno == ENOMEM ? CURLE_OUT_OF_MEMORY : CURLE_URL_MALFORMAT;
            return record_setopt_result(setup_error, "CURLOPT_PROXY", code);
        }
        int proxy_result = set_proxy_credentials(curl, setup_error, sanitized_proxy_url, proxy_credentials.username, proxy_credentials.password);
        free(sanitized_proxy_url);
        if (proxy_result != 0)
            return -1;
    }

    // FTP options
    if (bx_fetch_url_has_scheme(req->url, "ftp") || bx_fetch_url_has_scheme(req->url, "ftps")) {
        if (engine->cfg->ftp.no_passive_ftp) {
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FTPPORT, "-");  // Default behavior for active
        }
        else {
            // Default is passive, but we can ensure EPSV is on
            SETOPT_OR_RETURN(curl, setup_error, CURLOPT_FTP_USE_EPSV, 1L);
        }
    }

    if (engine->cfg->download.spider) {
        SETOPT_OR_RETURN(curl, setup_error, CURLOPT_NOBODY, 1L);
    }

    SETOPT_OR_RETURN(curl, setup_error, CURLOPT_PRIVATE, t);

    return 0;
}

#undef SETOPT_OR_RETURN

BxFetchError bx_fetch_map_curl_result(CURLcode code) {
    switch (code) {
        case CURLE_OK:
            return BX_FETCH_OK;
        case CURLE_WRITE_ERROR:
            return BX_FETCH_ERROR_IO;
        case CURLE_OPERATION_TIMEDOUT:
            return BX_FETCH_ERROR_TIMEOUT;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
#ifdef CURLE_SSL_CACERT_BADFILE
#if CURLE_SSL_CACERT_BADFILE != CURLE_PEER_FAILED_VERIFICATION
        case CURLE_SSL_CACERT_BADFILE:
#endif
#endif
#ifdef CURLE_SSL_CACERT
#if CURLE_SSL_CACERT != CURLE_PEER_FAILED_VERIFICATION
        case CURLE_SSL_CACERT:
#endif
#endif
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
            return BX_FETCH_ERROR_SSL;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_WEIRD_SERVER_REPLY:
            return BX_FETCH_ERROR_UNSUPPORTED;
        case CURLE_OUT_OF_MEMORY:
            return BX_FETCH_ERROR_MEMORY;
        default:
            return BX_FETCH_ERROR_NETWORK;
    }
}

BxFetchTransportErrorKind bx_fetch_classify_curl_transport_error(CURLcode code) {
    switch (code) {
        case CURLE_OK:
            return BX_FETCH_TRANSPORT_ERROR_NONE;
        case CURLE_SSL_CONNECT_ERROR:
            return BX_FETCH_TRANSPORT_ERROR_TLS_RETRYABLE;
        case CURLE_PEER_FAILED_VERIFICATION:
#ifdef CURLE_SSL_CACERT_BADFILE
#if CURLE_SSL_CACERT_BADFILE != CURLE_PEER_FAILED_VERIFICATION
        case CURLE_SSL_CACERT_BADFILE:
#endif
#endif
#ifdef CURLE_SSL_CACERT
#if CURLE_SSL_CACERT != CURLE_PEER_FAILED_VERIFICATION
        case CURLE_SSL_CACERT:
#endif
#endif
        case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
            return BX_FETCH_TRANSPORT_ERROR_TLS_FATAL;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
        case CURLE_TOO_MANY_REDIRECTS:
        case CURLE_WEIRD_SERVER_REPLY:
            return BX_FETCH_TRANSPORT_ERROR_PROTOCOL;
        default:
            return BX_FETCH_TRANSPORT_ERROR_NETWORK;
    }
}

int bx_fetch_engine_submit_with_setup_error(BxFetchEngine* engine,
                                            BxFetchRequest* req,
                                            BxFetchWriter* writer,
                                            BxFetchTransferHeadersCallback headers_cb,
                                            BxFetchTransferCallback cb,
                                            void* userdata,
                                            BxFetchRedirectPolicyCallback redirect_cb,
                                            void* redirect_userdata,
                                            BxFetchNetSetupError* setup_error) {
    if (setup_error)
        *setup_error = (BxFetchNetSetupError){.curl_code = -1, .error_number = -1};
    if (!engine || !req || !req->url || !writer)
        return -1;
    if (engine->cancelled) {
        errno = ECANCELED;
        return -1;
    }
    if (engine->quota_limit_bytes >= 0 && engine->quota_exhausted)
        return -1;
    if (!req->url_is_canonical) {
        char* canonical_url = bx_fetch_url_canonicalize(req->url);
        if (!canonical_url)
            return -1;
        free(req->url);
        req->url = canonical_url;
        req->url_is_canonical = true;
    }
    /*
     * Core policy rejects this earlier with an actionable error. Keep the
     * network boundary fail-closed for direct net API callers as well.
     */
    if (engine->cfg->http.paranoid && bx_fetch_url_has_userinfo(req->url)) {
        return -1;
    }
    if (bx_fetch_protocol_policy_evaluate_url(req->url, engine->cfg->https.https_only) != BX_FETCH_PROTOCOL_DECISION_ALLOW) {
        return -1;
    }

    char* display_url = bx_fetch_url_display_safe(req->url);
    if (!display_url)
        return -1;
    free(req->display_url);
    req->display_url = display_url;

    BxFetchTransfer* t = bx_fetch_transfer_new(req, writer);
    if (!t) {
        return -1;
    }
    if (!bx_fetch_net_require(engine, t->state == BX_FETCH_TRANSFER_STATE_INIT)) {
        t->req = NULL;
        bx_fetch_transfer_free(t);
        return -1;
    }
    t->engine = engine;
    t->resume_requested = bx_fetch_parse_resume_from_request(req, &t->resume_from);

    t->headers_cb = headers_cb;
    t->callback = cb;
    t->callback_userdata = userdata;
    t->redirect_cb = redirect_cb;
    t->redirect_userdata = redirect_userdata;

    if (setup_easy_handle(engine, t, setup_error) != 0) {
        t->req = NULL;
        bx_fetch_transfer_free(t);
        return -1;
    }

    CURLMcode add_rc = curl_multi_add_handle(engine->multi, t->easy);
    if (add_rc != CURLM_OK) {
        t->req = NULL;
        bx_fetch_transfer_free(t);
        return -1;
    }

    t->next_active = engine->active_head;
    engine->active_head = t;
    engine->active_transfers++;
    t->state = BX_FETCH_TRANSFER_STATE_ONGOING;
    return 0;
}

int bx_fetch_engine_submit(BxFetchEngine* engine,
                           BxFetchRequest* req,
                           BxFetchWriter* writer,
                           BxFetchTransferHeadersCallback headers_cb,
                           BxFetchTransferCallback cb,
                           void* userdata,
                           BxFetchRedirectPolicyCallback redirect_cb,
                           void* redirect_userdata) {
    return bx_fetch_engine_submit_with_setup_error(engine, req, writer, headers_cb, cb, userdata, redirect_cb, redirect_userdata, NULL);
}
