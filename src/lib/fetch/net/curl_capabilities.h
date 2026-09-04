#ifndef BX_FETCH_NET_CURL_CAPABILITIES_H
#define BX_FETCH_NET_CURL_CAPABILITIES_H

#include "lib/fetch/net.h"
#include <curl/curl.h>

/*
 * Validates normalized runtime configuration against the runtime-linked
 * libcurl. The probe handle is borrowed and must not carry user state whose
 * cleanup could publish or overwrite persistent data.
 */
int bx_fetch_net_validate_runtime_capabilities(const struct bx_fetch_config* cfg, const curl_version_info_data* version, CURL* probe, BxFetchNetSetupError* setup_error);

#endif
