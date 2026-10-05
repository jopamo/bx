#define _GNU_SOURCE
#include "engine_internal.h"
#include "credentials.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char* header_value(const BxFetchTransfer* transfer, const char* name) {
    size_t n = strlen(name);
    for (const struct curl_slist* h = transfer->headers; h; h = h->next) {
        if (strncasecmp(h->data, name, n) == 0 && (h->data[n] == ':' || h->data[n] == ';')) {
            const char* value = h->data + n + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            return value;
        }
    }
    return NULL;
}

bool bx_fetch_profile_automatic(const BxFetchTransfer* t) {
    const struct bx_fetch_config* cfg = t->engine->cfg;
    const BxFetchRequest* req = t->req;
    return cfg->http.client_profile == BX_FETCH_PROFILE_AUTO && !cfg->http.no_user_agent_fallback && !header_value(t, "User-Agent") && req->method &&
           (strcasecmp(req->method, "GET") == 0 || strcasecmp(req->method, "HEAD") == 0) && !req->body && !bx_fetch_request_has_body_file(req) && !cfg->http.bearer_token && !cfg->http.http_user &&
           !cfg->http.http_password && !cfg->download.user && !cfg->download.password && !cfg->http.load_cookies && !bx_fetch_prepared_url_has_userinfo(req->target) &&
           !bx_fetch_prepared_url_has_userinfo(t->current_target);
}

static bool append_header(struct curl_slist** headers, const char* value) {
    struct curl_slist* next = curl_slist_append(*headers, value);
    if (!next)
        return false;
    *headers = next;
    return true;
}

bool bx_fetch_profile_apply(BxFetchTransfer* t, const BxFetchPreparedUrl* target) {
    BxFetchClientProfile profile = t->engine->cfg->http.client_profile;
    BxFetchProtocol protocol = bx_fetch_prepared_url_protocol(target);
    if (protocol != BX_FETCH_PROTOCOL_HTTP && protocol != BX_FETCH_PROTOCOL_HTTPS)
        profile = BX_FETCH_PROFILE_LITERAL;
    if (profile == BX_FETCH_PROFILE_AUTO) {
        profile = BX_FETCH_PROFILE_CURL;
        if (bx_fetch_profile_automatic(t) && !bx_fetch_prepared_url_has_userinfo(target)) {
            if (!t->engine->profiles)
                t->engine->profiles = bx_fetch_profile_cache_new();
            profile = bx_fetch_profile_cache_get(t->engine->profiles, target);
            if (!t->engine->cfg->http.profile_agents[profile])
                profile = BX_FETCH_PROFILE_CURL;
            for (unsigned i = 0; i < t->profile_retries; i++)
                if (bx_fetch_prepared_url_same_origin(t->profile_retry_targets[i], target))
                    profile = t->retry_profiles[i];
        }
    }
    const char* custom = header_value(t, "User-Agent");
    if (custom)
        profile = BX_FETCH_PROFILE_LITERAL;
    const char* agent = custom ? custom : t->engine->cfg->http.user_agent;
    if (profile != BX_FETCH_PROFILE_LITERAL) {
        if (profile < BX_FETCH_PROFILE_CURL || profile >= BX_FETCH_PROFILE_AUTO || !t->engine->cfg->http.profile_agents[profile]) {
            errno = EINVAL;
            goto fail;
        }
        agent = t->engine->cfg->http.profile_agents[profile];
    }
    char* recorded = agent ? strdup(agent) : NULL;
    if (agent && !recorded)
        goto fail;
    bool browser = profile == BX_FETCH_PROFILE_CHROME || profile == BX_FETCH_PROFILE_FIREFOX;
    struct curl_slist* headers = NULL;
    for (const struct curl_slist* h = t->headers; h; h = h->next)
        if (!append_header(&headers, h->data))
            goto fail_headers;
    if (profile != BX_FETCH_PROFILE_LITERAL && !header_value(t, "Accept") &&
        !append_header(&headers, browser ? "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8" : "Accept: */*"))
        goto fail_headers;
    if (browser && !header_value(t, "Accept-Language") && !append_header(&headers, "Accept-Language: en-US,en;q=0.5"))
        goto fail_headers;
    if (curl_easy_setopt(t->easy, CURLOPT_USERAGENT, agent) != CURLE_OK || curl_easy_setopt(t->easy, CURLOPT_HTTPHEADER, headers) != CURLE_OK) {
        errno = EIO;
        goto fail_headers;
    }
    curl_slist_free_all(t->profile_headers);
    t->profile_headers = headers;
    free(t->resp->user_agent);
    t->resp->user_agent = recorded;
    t->profile = profile;
    return true;
fail_headers:
    curl_slist_free_all(headers);
    free(recorded);
fail:
    bx_fetch_transfer_mark_io_failure(t, ENOMEM);
    return false;
}

bool bx_fetch_profile_retry(BxFetchTransfer* t, int status) {
    if (!bx_fetch_profile_automatic(t) || t->engine->cfg->download.tries <= 1 || t->profile_retries >= 2 || (status != 403 && status != 406) || t->profile < BX_FETCH_PROFILE_CURL ||
        t->profile > BX_FETCH_PROFILE_FIREFOX || t->io_failed || t->resume_validation_failed || t->save_headers_written || t->anubis_phase != BX_FETCH_ANUBIS_PHASE_NONE ||
        bx_fetch_response_header_value(t->resp, "WWW-Authenticate") || bx_fetch_response_header_value(t->resp, "Retry-After") || bx_fetch_response_header_value(t->resp, "X-RateLimit-Remaining") ||
        bx_fetch_response_header_value(t->resp, "RateLimit-Remaining"))
        return false;
    BxFetchEngine* engine = t->engine;
    if (!bx_fetch_request_budget_check(t) || (engine->budget->max_requests > 0 && engine->budget->requests_started >= (uint64_t)engine->budget->max_requests))
        return false;
    unsigned tried = 1u << t->profile;
    /* Include each attempted preference, not only the most recent origin. */
    for (unsigned i = 0; i < t->profile_retries; i++)
        if (bx_fetch_prepared_url_same_origin(t->profile_retry_targets[i], t->current_target))
            tried |= (1u << t->retry_profiles[i]) | (1u << t->rejected_profiles[i]);
    BxFetchClientProfile next = BX_FETCH_PROFILE_CURL;
    while (next <= BX_FETCH_PROFILE_FIREFOX && ((tried & (1u << next)) || !engine->cfg->http.profile_agents[next]))
        next++;
    if (next > BX_FETCH_PROFILE_FIREFOX)
        return false;

    BxFetchPreparedUrl* rejected = bx_fetch_prepared_url_clone(t->current_target);
    BxFetchPreparedUrl* target = bx_fetch_prepared_url_clone(t->req->target);
    BxFetchResponse* response = bx_fetch_response_new();
    if (!rejected || !target || !response) {
        bx_fetch_prepared_url_free(rejected);
        bx_fetch_prepared_url_free(target);
        bx_fetch_response_free(response);
        bx_fetch_transfer_mark_io_failure(t, ENOMEM);
        return false;
    }
    /* Restart at the original authority, preserving libcurl's credential scope. */
    if ((!engine->cfg->http.no_cookies && bx_fetch_net_scope_cookies(t->easy, t->current_target, target) != 0) || curl_multi_remove_handle(engine->multi, t->easy) != CURLM_OK) {
        bx_fetch_prepared_url_free(rejected);
        bx_fetch_prepared_url_free(target);
        bx_fetch_response_free(response);
        bx_fetch_transfer_mark_io_failure(t, EIO);
        return false;
    }
    t->multi_attached = false;
    bx_fetch_transfer_reset_response_state(t);
    bx_fetch_response_free(t->resp);
    t->resp = response;
    response->used_spider_get = t->spider_get;
    bx_fetch_prepared_url_free(t->current_target);
    t->current_target = target;
    bx_fetch_prepared_url_free(t->pending_redirect_target);
    t->pending_redirect_target = NULL;
    t->profile_retry_targets[t->profile_retries] = rejected;
    t->retry_profiles[t->profile_retries] = next;
    t->rejected_profiles[t->profile_retries] = t->profile;
    t->profile_retries++;
    if (curl_easy_setopt(t->easy, CURLOPT_URL, bx_fetch_prepared_url_transport(target)) != CURLE_OK ||
        curl_easy_setopt(t->easy, CURLOPT_TIMEOUT_MS, bx_fetch_request_budget_timeout_ms(engine)) != CURLE_OK || !bx_fetch_profile_apply(t, target) ||
        curl_multi_add_handle(engine->multi, t->easy) != CURLM_OK) {
        bx_fetch_transfer_mark_io_failure(t, EIO);
        return false;
    }
    t->multi_attached = true;
    return true;
}

void bx_fetch_profile_learn(BxFetchTransfer* t) {
    bool negotiated = t->profile == BX_FETCH_PROFILE_CURL;
    for (unsigned i = 0; i < t->profile_retries; i++)
        if (bx_fetch_prepared_url_same_origin(t->profile_retry_targets[i], t->current_target))
            negotiated = true;
    /* A cached request finishing after expiry must not renew that preference. */
    if (!negotiated)
        return;
    if (bx_fetch_profile_automatic(t) && t->anubis_phase == BX_FETCH_ANUBIS_PHASE_NONE && ((t->resp->status_code >= 200 && t->resp->status_code < 300) || t->resp->status_code == 304))
        bx_fetch_profile_cache_put(t->engine->profiles, t->current_target, t->profile);
}
