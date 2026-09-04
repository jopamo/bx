#define _GNU_SOURCE
#include "lib/fetch/filter.h"
#include "lib/fetch/regex.h"
#include "lib/fetch/url.h"
#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <string.h>

struct Filter {
    const struct bx_fetch_config* cfg;
    char** accept_exts;
    int accept_count;
    char** reject_exts;
    int reject_count;
    char** accept_domains;
    int accept_domain_count;
    char** reject_domains;
    int reject_domain_count;
    char** include_dirs;
    int include_dir_count;
    char** exclude_dirs;
    int exclude_dir_count;
    char** seed_hosts;
    int seed_host_count;
    int seed_host_capacity;
    regex_t accept_regex;
    bool accept_regex_compiled;
};

static char** split_list(const char* list, int* count) {
    *count = 0;
    if (!list || list[0] == '\0') {
        return NULL;
    }

    char* copy = strdup(list);
    if (!copy) {
        *count = -1;
        return NULL;
    }

    char** res = NULL;
    int used = 0;
    int capacity = 0;
    char* saveptr = NULL;
    for (char* token = strtok_r(copy, ",", &saveptr); token != NULL; token = strtok_r(NULL, ",", &saveptr)) {
        while (*token && isspace((unsigned char)*token))
            token++;

        char* end = token + strlen(token);
        while (end > token && isspace((unsigned char)end[-1])) {
            end--;
        }
        *end = '\0';

        if (token[0] == '\0')
            continue;

        if (used == capacity) {
            int next_capacity = capacity == 0 ? 4 : capacity * 2;
            char** grown = realloc(res, (size_t)next_capacity * sizeof(char*));
            if (!grown) {
                for (int i = 0; i < used; i++)
                    free(res[i]);
                free(res);
                free(copy);
                *count = -1;
                return NULL;
            }
            res = grown;
            capacity = next_capacity;
        }

        res[used] = strdup(token);
        if (!res[used]) {
            for (int i = 0; i < used; i++)
                free(res[i]);
            free(res);
            free(copy);
            *count = -1;
            return NULL;
        }
        used++;
    }

    free(copy);
    if (used == 0) {
        free(res);
        return NULL;
    }

    *count = used;
    return res;
}

static bool host_matches_rule(const char* host, const char* rule, bool include_subdomains) {
    if (!host || !rule)
        return false;

    if (strcasecmp(host, rule) == 0)
        return true;
    if (!include_subdomains)
        return false;

    size_t host_len = strlen(host);
    size_t rule_len = strlen(rule);
    if (host_len <= rule_len)
        return false;
    if (host[host_len - rule_len - 1] != '.')
        return false;

    return strcasecmp(host + host_len - rule_len, rule) == 0;
}

static bool host_matches_any(const char* host, char** rules, int count, bool include_subdomains) {
    for (int i = 0; i < count; i++) {
        if (host_matches_rule(host, rules[i], include_subdomains)) {
            return true;
        }
    }
    return false;
}

static bool path_matches_directory_rule(const char* path, const char* rule) {
    if (!path || !rule)
        return false;

    while (*path == '/')
        path++;
    while (*rule == '/')
        rule++;

    size_t rule_len = strlen(rule);
    while (rule_len > 0 && rule[rule_len - 1] == '/')
        rule_len--;
    if (rule_len == 0)
        return true;
    if (strncmp(path, rule, rule_len) != 0)
        return false;
    return path[rule_len] == '\0' || path[rule_len] == '/';
}

static bool path_matches_any_directory(const char* path, char** rules, int count) {
    for (int i = 0; i < count; i++) {
        if (path_matches_directory_rule(path, rules[i]))
            return true;
    }
    return false;
}

static int append_seed_host(Filter* f, const char* host) {
    if (!f || !host || host[0] == '\0')
        return -1;
    if (host_matches_any(host, f->seed_hosts, f->seed_host_count, false))
        return 0;

    if (f->seed_host_count == f->seed_host_capacity) {
        int next_capacity = f->seed_host_capacity == 0 ? 4 : f->seed_host_capacity * 2;
        char** grown = realloc(f->seed_hosts, (size_t)next_capacity * sizeof(char*));
        if (!grown)
            return -1;
        f->seed_hosts = grown;
        f->seed_host_capacity = next_capacity;
    }

    f->seed_hosts[f->seed_host_count] = strdup(host);
    if (!f->seed_hosts[f->seed_host_count])
        return -1;
    f->seed_host_count++;
    return 0;
}

static int compile_accept_regex(Filter* f) {
    if (!f || !f->cfg || !f->cfg->recursive.accept_regex)
        return 0;

    int flags = 0;
    if (bx_fetch_regex_compile_flags_for_type(f->cfg->recursive.regex_type, &flags) != 0) {
        return -1;
    }

    if (regcomp(&f->accept_regex, f->cfg->recursive.accept_regex, flags) != 0) {
        return -1;
    }

    f->accept_regex_compiled = true;
    return 0;
}

static bool url_matches_accept_regex(Filter* f, const char* url) {
    if (!f || !f->accept_regex_compiled || !url)
        return true;
    return regexec(&f->accept_regex, url, 0, NULL, 0) == 0;
}

Filter* bx_fetch_filter_new(const struct bx_fetch_config* cfg) {
    Filter* f = calloc(1, sizeof(Filter));
    if (!f)
        return NULL;

    f->cfg = cfg;
    f->accept_exts = split_list(cfg->recursive.accept_list, &f->accept_count);
    f->reject_exts = split_list(cfg->recursive.reject_list, &f->reject_count);
    f->accept_domains = split_list(cfg->recursive.domains, &f->accept_domain_count);
    f->reject_domains = split_list(cfg->recursive.exclude_domains, &f->reject_domain_count);
    f->include_dirs = split_list(cfg->recursive.include_directories, &f->include_dir_count);
    f->exclude_dirs = split_list(cfg->recursive.exclude_directories, &f->exclude_dir_count);
    if (f->accept_count < 0 || f->reject_count < 0 || f->accept_domain_count < 0 || f->reject_domain_count < 0 || f->include_dir_count < 0 || f->exclude_dir_count < 0) {
        bx_fetch_filter_free(f);
        return NULL;
    }

    if (compile_accept_regex(f) != 0) {
        bx_fetch_filter_free(f);
        return NULL;
    }

    return f;
}

void bx_fetch_filter_free(Filter* f) {
    if (!f)
        return;
    for (int i = 0; i < f->accept_count; i++)
        free(f->accept_exts[i]);
    free(f->accept_exts);
    for (int i = 0; i < f->reject_count; i++)
        free(f->reject_exts[i]);
    free(f->reject_exts);
    for (int i = 0; i < f->accept_domain_count; i++)
        free(f->accept_domains[i]);
    free(f->accept_domains);
    for (int i = 0; i < f->reject_domain_count; i++)
        free(f->reject_domains[i]);
    free(f->reject_domains);
    for (int i = 0; i < f->include_dir_count; i++)
        free(f->include_dirs[i]);
    free(f->include_dirs);
    for (int i = 0; i < f->exclude_dir_count; i++)
        free(f->exclude_dirs[i]);
    free(f->exclude_dirs);
    for (int i = 0; i < f->seed_host_count; i++)
        free(f->seed_hosts[i]);
    free(f->seed_hosts);
    if (f->accept_regex_compiled) {
        regfree(&f->accept_regex);
    }
    free(f);
}

int bx_fetch_filter_add_seed_url(Filter* f, const char* url) {
    if (!f || !url)
        return -1;

    char* canonical = bx_fetch_url_canonicalize(url);
    if (!canonical)
        return -1;
    int rc = bx_fetch_filter_add_canonical_seed_url(f, canonical);
    free(canonical);
    return rc;
}

int bx_fetch_filter_add_canonical_seed_url(Filter* f, const char* canonical_url) {
    if (!f || !canonical_url)
        return -1;

    MiraURL* mu = bx_fetch_url_parse(canonical_url);
    if (!mu)
        return -1;

    int rc = -1;
    if (mu->host && mu->host[0] != '\0') {
        rc = append_seed_host(f, mu->host);
    }

    bx_fetch_url_free(mu);
    return rc;
}

static bool has_extension(const char* url, const char* ext) {
    const char* dot = strrchr(url, '.');
    if (!dot)
        return false;

    const char* slash = strrchr(url, '/');
    if (slash && dot < slash)
        return false;

    const char* query = strchr(dot + 1, '?');
    const char* fragment = strchr(dot + 1, '#');
    const char* end = NULL;
    if (query && fragment) {
        end = (query < fragment) ? query : fragment;
    }
    else if (query) {
        end = query;
    }
    else if (fragment) {
        end = fragment;
    }

    const char* ext_start = dot + 1;
    size_t ext_len = end ? (size_t)(end - ext_start) : strlen(ext_start);
    if (ext_len == 0)
        return false;
    if (strlen(ext) != ext_len)
        return false;

    return strncasecmp(ext_start, ext, ext_len) == 0;
}

FilterDecision bx_fetch_filter_evaluate_url(Filter* f, const char* url) {
    if (!f)
        return FILTER_DECISION_ACCEPT;

    MiraProtocolDecision protocol_decision = bx_fetch_protocol_policy_evaluate_url(url, f->cfg->https.https_only);
    if (protocol_decision == BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED) {
        return FILTER_DECISION_UNSUPPORTED_PROTOCOL;
    }
    if (protocol_decision == BX_FETCH_PROTOCOL_DECISION_HTTPS_ONLY) {
        return FILTER_DECISION_HTTPS_ONLY;
    }

    char* canonical = bx_fetch_url_canonicalize(url);
    if (!canonical)
        return FILTER_DECISION_INVALID_URL;
    FilterDecision decision = bx_fetch_filter_evaluate_canonical_url(f, canonical);
    free(canonical);
    return decision;
}

static FilterDecision evaluate_transport_policy(const Filter* f, const MiraURL* url, const char* canonical_url) {
    if (!f || !url)
        return FILTER_DECISION_ACCEPT;

    MiraProtocolDecision protocol_decision = bx_fetch_protocol_policy_evaluate_scheme(url->scheme, f->cfg->https.https_only);
    if (protocol_decision == BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED || protocol_decision == BX_FETCH_PROTOCOL_DECISION_INVALID_URL) {
        return FILTER_DECISION_UNSUPPORTED_PROTOCOL;
    }
    if (protocol_decision == BX_FETCH_PROTOCOL_DECISION_HTTPS_ONLY) {
        return FILTER_DECISION_HTTPS_ONLY;
    }
    if (f->cfg->http.paranoid && bx_fetch_url_has_userinfo(canonical_url)) {
        return FILTER_DECISION_URL_CREDENTIALS;
    }
    return FILTER_DECISION_ACCEPT;
}

FilterDecision bx_fetch_filter_evaluate_transport_canonical_url(Filter* f, const char* canonical_url) {
    if (!f)
        return FILTER_DECISION_ACCEPT;

    MiraURL* url = bx_fetch_url_parse(canonical_url);
    if (!url)
        return FILTER_DECISION_INVALID_URL;
    FilterDecision decision = evaluate_transport_policy(f, url, canonical_url);
    bx_fetch_url_free(url);
    return decision;
}

FilterDecision bx_fetch_filter_evaluate_canonical_url(Filter* f, const char* canonical_url) {
    if (!f)
        return FILTER_DECISION_ACCEPT;

    MiraURL* mu = bx_fetch_url_parse(canonical_url);
    if (!mu)
        return FILTER_DECISION_INVALID_URL;

    FilterDecision decision = evaluate_transport_policy(f, mu, canonical_url);

    if (decision == FILTER_DECISION_ACCEPT) {
        const bool exact_seed_match = mu->host && host_matches_any(mu->host, f->seed_hosts, f->seed_host_count, false);
        const bool explicit_domain_match = mu->host && host_matches_any(mu->host, f->accept_domains, f->accept_domain_count, true);

        if (!mu->host || mu->host[0] == '\0') {
            if (f->reject_domain_count > 0 || f->accept_domain_count > 0 || (!f->cfg->recursive.span_hosts && f->seed_host_count > 0)) {
                decision = FILTER_DECISION_DOMAIN_SCOPE;
            }
        }
        else if (host_matches_any(mu->host, f->reject_domains, f->reject_domain_count, true)) {
            decision = FILTER_DECISION_DOMAIN_DENYLIST;
        }
        else if (f->accept_domain_count > 0) {
            if (!(explicit_domain_match || exact_seed_match)) {
                decision = FILTER_DECISION_DOMAIN_SCOPE;
            }
        }
        else if (!f->cfg->recursive.span_hosts && f->seed_host_count > 0) {
            if (!exact_seed_match) {
                decision = FILTER_DECISION_DOMAIN_SCOPE;
            }
        }
    }

    if (decision == FILTER_DECISION_ACCEPT) {
        if (path_matches_any_directory(mu->path, f->exclude_dirs, f->exclude_dir_count)) {
            decision = FILTER_DECISION_DIRECTORY_DENYLIST;
        }
        else if (f->include_dir_count > 0 && !path_matches_any_directory(mu->path, f->include_dirs, f->include_dir_count)) {
            decision = FILTER_DECISION_DIRECTORY_SCOPE;
        }
    }

    // Extension filtering
    if (decision == FILTER_DECISION_ACCEPT) {
        if (f->reject_count > 0) {
            for (int i = 0; i < f->reject_count; i++) {
                if (has_extension(canonical_url, f->reject_exts[i])) {
                    decision = FILTER_DECISION_SUFFIX_DENYLIST;
                    break;
                }
            }
        }
        if (decision == FILTER_DECISION_ACCEPT && f->accept_regex_compiled && !url_matches_accept_regex(f, canonical_url)) {
            decision = FILTER_DECISION_REGEX_ALLOWLIST;
        }
        if (decision == FILTER_DECISION_ACCEPT && f->accept_count > 0) {
            bool found = false;
            for (int i = 0; i < f->accept_count; i++) {
                if (has_extension(canonical_url, f->accept_exts[i])) {
                    found = true;
                    break;
                }
            }
            if (!found)
                decision = FILTER_DECISION_SUFFIX_ALLOWLIST;
        }
    }

    bx_fetch_url_free(mu);
    return decision;
}

const char* bx_fetch_filter_decision_reason(FilterDecision decision) {
    switch (decision) {
        case FILTER_DECISION_ACCEPT:
            return NULL;
        case FILTER_DECISION_INVALID_URL:
            return "invalid-url";
        case FILTER_DECISION_UNSUPPORTED_PROTOCOL:
            return "unsupported-protocol";
        case FILTER_DECISION_HTTPS_ONLY:
            return "https-only";
        case FILTER_DECISION_URL_CREDENTIALS:
            return "url-credentials";
        case FILTER_DECISION_DOMAIN_DENYLIST:
            return "exclude-domains";
        case FILTER_DECISION_DOMAIN_SCOPE:
            return "host-scope";
        case FILTER_DECISION_DIRECTORY_DENYLIST:
            return "exclude-directories";
        case FILTER_DECISION_DIRECTORY_SCOPE:
            return "include-directories";
        case FILTER_DECISION_SUFFIX_DENYLIST:
            return "reject-list";
        case FILTER_DECISION_REGEX_ALLOWLIST:
            return "accept-regex";
        case FILTER_DECISION_SUFFIX_ALLOWLIST:
            return "accept-list";
    }

    return "unspecified";
}

bool bx_fetch_filter_url_accepted(Filter* f, const char* url) {
    return bx_fetch_filter_evaluate_url(f, url) == FILTER_DECISION_ACCEPT;
}
