#ifndef BX_FETCH_NET_CREDENTIALS_H
#define BX_FETCH_NET_CREDENTIALS_H

#include "lib/fetch/config.h"
#include "lib/fetch/url.h"

typedef enum {
    BX_FETCH_CREDENTIAL_SOURCE_NONE = 0,
    BX_FETCH_CREDENTIAL_SOURCE_URL,
    BX_FETCH_CREDENTIAL_SOURCE_GENERIC,
    BX_FETCH_CREDENTIAL_SOURCE_HTTP,
    BX_FETCH_CREDENTIAL_SOURCE_FTP,
    BX_FETCH_CREDENTIAL_SOURCE_PROXY_CONFIG,
    BX_FETCH_CREDENTIAL_SOURCE_PROXY_INHERITED,
} BxFetchCredentialSource;

typedef struct {
    BxFetchCredentialSource source;
    const char* username;
    const char* password;
} BxFetchCredentialSelection;

/*
 * Selection strings are borrowed from cfg or static empty strings. A selected
 * configured source is atomic: an omitted component becomes empty and is
 * never inherited from a lower-priority source.
 */
void bx_fetch_net_select_origin_credentials(const struct bx_fetch_config* cfg, const BxFetchPreparedUrl* target, BxFetchCredentialSelection* selection);
void bx_fetch_net_select_proxy_credentials(const struct bx_fetch_config* cfg, BxFetchCredentialSelection* selection);
/* Returns the borrowed proxy environment value selected for protocol. */
const char* bx_fetch_net_proxy_environment_url(BxFetchProtocol protocol);
/*
 * If proxy_url contains authority userinfo, returns an owned equivalent with
 * userinfo removed in `sanitized_out`; otherwise leaves it NULL. Returns -1
 * when userinfo is present but the proxy URL cannot be represented safely.
 */
int bx_fetch_net_sanitize_proxy_url_for_explicit_credentials(const char* proxy_url, char** sanitized_out);

#endif
