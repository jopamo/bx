#ifndef BX_FETCH_URL_H
#define BX_FETCH_URL_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, core, policy, crawl, fs, net, store */

/*
 * Layering contract:
 * - URL parsing/resolution/canonicalization primitives are centralized here so
 *   dedupe, filtering, crawl, and net use one canonical implementation path.
 * - The protocol allowlist is authoritative for both policy checks and
 *   transport restrictions. `https_only` may narrow, but never widen, it.
 *
 * Ownership and lifetime:
 * - bx_fetch_url_parse() returns an owned BxFetchUrl released by bx_fetch_url_free().
 * - String-returning helpers return heap strings owned by the caller.
 * - Input URL pointers are borrowed and never retained.
 * - bx_fetch_url_canonicalize() returns a fragment-free request identity.
 * - bx_fetch_url_display_safe() returns canonical URL text with all authority
 *   userinfo removed. Observable and persistent boundaries must use this
 *   representation and must not fall back to the input URL on failure.
 */

#include <stdbool.h>
#include <stddef.h>

#define BX_FETCH_URL_DISPLAY_REDACTED "[URL redacted]"

typedef enum {
    BX_FETCH_PROTOCOL_NONE = 0,
    BX_FETCH_PROTOCOL_HTTP = 1u << 0,
    BX_FETCH_PROTOCOL_HTTPS = 1u << 1,
    BX_FETCH_PROTOCOL_FTP = 1u << 2,
    BX_FETCH_PROTOCOL_FTPS = 1u << 3,
} BxFetchProtocol;

typedef enum {
    BX_FETCH_PROTOCOL_DECISION_ALLOW = 0,
    BX_FETCH_PROTOCOL_DECISION_INVALID_URL,
    BX_FETCH_PROTOCOL_DECISION_UNSUPPORTED,
    BX_FETCH_PROTOCOL_DECISION_HTTPS_ONLY,
} BxFetchProtocolDecision;

typedef struct {
    char* scheme;
    char* user;
    char* password;
    char* host;
    int port;
    char* path;
    char* query;
    char* fragment;
} BxFetchUrl;

/*
 * Immutable normalized URL state carried through planning, scheduling, and
 * transport. The transport identity may contain userinfo; the display
 * identity never does. Callers cannot mutate either representation
 * independently.
 */
typedef struct BxFetchPreparedUrl BxFetchPreparedUrl;

BxFetchUrl* bx_fetch_url_parse(const char* url);
void bx_fetch_url_free(BxFetchUrl* mu);
char* bx_fetch_url_resolve(const char* base_url, const char* relative_url);
char* bx_fetch_url_resolve_canonical(const char* base_url, const char* relative_url);
char* bx_fetch_url_to_string(BxFetchUrl* mu);
char* bx_fetch_url_canonicalize(const char* url);
char* bx_fetch_url_display_safe(const char* url);
bool bx_fetch_url_has_scheme(const char* url, const char* scheme);
bool bx_fetch_url_has_userinfo(const char* url);

/* Normalizes untrusted input and validates the shared protocol allowlist. */
BxFetchPreparedUrl* bx_fetch_url_prepare(const char* url);
/*
 * Internal fast path for already canonical, fragment-free URL text. It still
 * parses and validates protocol/origin state but does not normalize again.
 */
BxFetchPreparedUrl* bx_fetch_url_prepare_canonical(const char* canonical_url);
BxFetchPreparedUrl* bx_fetch_prepared_url_clone(const BxFetchPreparedUrl* url);
void bx_fetch_prepared_url_free(BxFetchPreparedUrl* url);
/* Resolves one hostile redirect/link reference and returns normalized state. */
BxFetchPreparedUrl* bx_fetch_prepared_url_resolve(const BxFetchPreparedUrl* base, const char* reference);
const char* bx_fetch_prepared_url_transport(const BxFetchPreparedUrl* url);
const char* bx_fetch_prepared_url_display(const BxFetchPreparedUrl* url);
const char* bx_fetch_prepared_url_scheme(const BxFetchPreparedUrl* url);
const char* bx_fetch_prepared_url_host(const BxFetchPreparedUrl* url);
int bx_fetch_prepared_url_port(const BxFetchPreparedUrl* url);
BxFetchProtocol bx_fetch_prepared_url_protocol(const BxFetchPreparedUrl* url);
bool bx_fetch_prepared_url_has_userinfo(const BxFetchPreparedUrl* url);
bool bx_fetch_prepared_url_same_origin(const BxFetchPreparedUrl* left, const BxFetchPreparedUrl* right);
BxFetchProtocolDecision bx_fetch_prepared_url_policy(const BxFetchPreparedUrl* url, bool https_only);

BxFetchProtocol bx_fetch_protocol_from_scheme(const char* scheme);
unsigned int bx_fetch_protocol_policy_mask(bool https_only);
BxFetchProtocolDecision bx_fetch_protocol_policy_evaluate_scheme(const char* scheme, bool https_only);
BxFetchProtocolDecision bx_fetch_protocol_policy_evaluate_url(const char* url, bool https_only);
const char* bx_fetch_protocol_decision_reason(BxFetchProtocolDecision decision);
bool bx_fetch_protocol_policy_format(bool https_only, char* out, size_t out_size);

#endif  // BX_FETCH_URL_H
