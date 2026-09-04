#ifndef MIRA_NET_CREDENTIALS_H
#define MIRA_NET_CREDENTIALS_H

#include "lib/fetch/config.h"

typedef enum {
    MIRA_CREDENTIAL_SOURCE_NONE = 0,
    MIRA_CREDENTIAL_SOURCE_URL,
    MIRA_CREDENTIAL_SOURCE_GENERIC,
    MIRA_CREDENTIAL_SOURCE_HTTP,
    MIRA_CREDENTIAL_SOURCE_FTP,
    MIRA_CREDENTIAL_SOURCE_PROXY_CONFIG,
    MIRA_CREDENTIAL_SOURCE_PROXY_INHERITED,
} MiraCredentialSource;

typedef struct {
    MiraCredentialSource source;
    const char *username;
    const char *password;
} MiraCredentialSelection;

/*
 * Selection strings are borrowed from cfg or static empty strings. A selected
 * configured source is atomic: an omitted component becomes empty and is
 * never inherited from a lower-priority source.
 */
void net_select_origin_credentials(const EffectiveConfig *cfg,
                                   const char *url,
                                   MiraCredentialSelection *selection);
void net_select_proxy_credentials(const EffectiveConfig *cfg,
                                  MiraCredentialSelection *selection);
/* Returns the borrowed proxy environment value selected for request_url. */
const char *net_proxy_environment_url(const char *request_url);
/*
 * If proxy_url contains authority userinfo, returns an owned equivalent with
 * userinfo removed in `sanitized_out`; otherwise leaves it NULL. Returns -1
 * when userinfo is present but the proxy URL cannot be represented safely.
 */
int net_sanitize_proxy_url_for_explicit_credentials(
    const char *proxy_url, char **sanitized_out);

#endif
