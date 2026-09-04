#include "curl_capabilities.h"
#include <curl/curl.h>
#include <errno.h>

static int global_failure(BxFetchNetSetupError* setup_error, const char* capability, CURLcode curl_code, int error_number) {
    if (setup_error) {
        setup_error->present = true;
        setup_error->capability = capability;
        setup_error->curl_code = (int)curl_code;
        setup_error->error_number = error_number;
    }
    return -1;
}

int bx_fetch_global_init(const struct bx_fetch_config* cfg, BxFetchNetSetupError* setup_error) {
    if (setup_error)
        *setup_error = (BxFetchNetSetupError){.curl_code = -1, .error_number = -1};
    if (!cfg) {
        errno = EINVAL;
        return global_failure(setup_error, "valid fetch configuration", CURLE_BAD_FUNCTION_ARGUMENT, EINVAL);
    }

    CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (result != CURLE_OK)
        return global_failure(setup_error, "libcurl global initialization", result, -1);

    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    if (!version) {
        curl_global_cleanup();
        return global_failure(setup_error, "linked libcurl capability data", CURLE_FAILED_INIT, -1);
    }

    CURL* probe = curl_easy_init();
    if (!probe) {
        curl_global_cleanup();
        return global_failure(setup_error, "libcurl capability probe", CURLE_OUT_OF_MEMORY, ENOMEM);
    }

    int validation_result = bx_fetch_net_validate_runtime_capabilities(cfg, version, probe, setup_error);
    curl_easy_cleanup(probe);
    if (validation_result != 0) {
        curl_global_cleanup();
        return -1;
    }
    return 0;
}

void bx_fetch_global_cleanup(void) {
    curl_global_cleanup();
}
