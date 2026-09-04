#ifndef MIRA_URL_H
#define MIRA_URL_H

/* MIRA_HEADER_OWNER: runtime */
/* MIRA_HEADER_CONSUMERS: runtime, core, policy, crawl, fs, net, store */

/*
 * Layering contract:
 * - URL parsing/resolution/canonicalization primitives are centralized here so
 *   dedupe, filtering, crawl, and net use one canonical implementation path.
 * - The protocol allowlist is authoritative for both policy checks and
 *   transport restrictions. `https_only` may narrow, but never widen, it.
 *
 * Ownership and lifetime:
 * - mira_url_parse() returns an owned MiraURL released by mira_url_free().
 * - String-returning helpers return heap strings owned by the caller.
 * - Input URL pointers are borrowed and never retained.
 * - mira_url_canonicalize() returns a fragment-free request identity.
 * - mira_url_display_safe() returns canonical URL text with all authority
 *   userinfo removed. Observable and persistent boundaries must use this
 *   representation and must not fall back to the input URL on failure.
 */

#include <stdbool.h>
#include <stddef.h>

#define MIRA_URL_DISPLAY_REDACTED "[URL redacted]"

typedef enum {
    MIRA_PROTOCOL_NONE = 0,
    MIRA_PROTOCOL_HTTP = 1u << 0,
    MIRA_PROTOCOL_HTTPS = 1u << 1,
    MIRA_PROTOCOL_FTP = 1u << 2,
    MIRA_PROTOCOL_FTPS = 1u << 3,
} MiraProtocol;

typedef enum {
    MIRA_PROTOCOL_DECISION_ALLOW = 0,
    MIRA_PROTOCOL_DECISION_INVALID_URL,
    MIRA_PROTOCOL_DECISION_UNSUPPORTED,
    MIRA_PROTOCOL_DECISION_HTTPS_ONLY,
} MiraProtocolDecision;

typedef struct {
    char* scheme;
    char* user;
    char* password;
    char* host;
    int port;
    char* path;
    char* query;
    char* fragment;
} MiraURL;

MiraURL* mira_url_parse(const char* url);
void mira_url_free(MiraURL* mu);
char* mira_url_resolve(const char* base_url, const char* relative_url);
char* mira_url_resolve_canonical(const char* base_url, const char* relative_url);
char* mira_url_to_string(MiraURL* mu);
char* mira_url_canonicalize(const char* url);
char* mira_url_display_safe(const char* url);
bool mira_url_has_scheme(const char* url, const char* scheme);
bool mira_url_has_userinfo(const char* url);

MiraProtocol mira_protocol_from_scheme(const char* scheme);
unsigned int mira_protocol_policy_mask(bool https_only);
MiraProtocolDecision mira_protocol_policy_evaluate_scheme(const char* scheme, bool https_only);
MiraProtocolDecision mira_protocol_policy_evaluate_url(const char* url, bool https_only);
const char* mira_protocol_decision_reason(MiraProtocolDecision decision);
bool mira_protocol_policy_format(bool https_only, char* out, size_t out_size);

#endif  // MIRA_URL_H
