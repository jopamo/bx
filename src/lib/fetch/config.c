#define _GNU_SOURCE
#include "lib/fetch/config.h"
#include "lib/fetch/resource_limits.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct bx_fetch_config* bx_fetch_config_new(void) {
    struct bx_fetch_config* cfg = calloc(1, sizeof(struct bx_fetch_config));
    if (!cfg)
        return NULL;

    // Set defaults
    cfg->download.tries = 20;
    cfg->logging.verbosity = BX_FETCH_VERBOSITY_VERBOSE;
    cfg->logging.structured_errors = true;
    cfg->download.dns_timeout = 60;
    cfg->download.connect_timeout = 60;
    cfg->download.read_timeout = 60;
    cfg->download.wait = 0;
    cfg->download.waitretry = 10;
    cfg->download.max_threads = 5;
    cfg->download.quota = -1;  // unlimited unless explicitly configured
    cfg->download.show_progress = true;
    cfg->recursive.level = 5;  // default recursion depth
    cfg->http.max_redirect = 20;
    cfg->http.redirect_method = strdup("legacy");
    cfg->http.user_agent = strdup("mira/0.1.0");
    cfg->download.prefer_family = strdup("none");
    if (!cfg->http.redirect_method || !cfg->http.user_agent || !cfg->download.prefer_family) {
        bx_fetch_config_free(cfg);
        return NULL;
    }

    return cfg;
}

int bx_fetch_config_copy_urls(struct bx_fetch_config* cfg, int count, char* const* urls) {
    if (!cfg || count < 0 || (count > 0 && !urls)) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)count > BX_FETCH_URL_STATE_MAX_ENTRIES) {
        errno = EFBIG;
        return -1;
    }

    char** copies = count > 0 ? calloc((size_t)count, sizeof(*copies)) : NULL;
    if (count > 0 && !copies)
        return -1;

    size_t retained_bytes = 0;
    for (int index = 0; index < count; index++) {
        if (!urls[index]) {
            errno = EINVAL;
            goto fail;
        }
        size_t length = 0;
        if (!bx_fetch_resource_bounded_strlen(urls[index], BX_FETCH_URL_MAX_BYTES, &length) ||
            !bx_fetch_resource_can_reserve((size_t)index, retained_bytes, 1u, length, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
            errno = EFBIG;
            goto fail;
        }
        copies[index] = strdup(urls[index]);
        if (!copies[index])
            goto fail;
        retained_bytes += length;
    }

    if (cfg->input.urls) {
        for (int index = 0; index < cfg->input.url_count; index++)
            free(cfg->input.urls[index]);
    }
    free(cfg->input.urls);
    cfg->input.urls = copies;
    cfg->input.url_count = count;
    return 0;

fail: {
    int error_number = errno ? errno : ENOMEM;
    for (int index = 0; index < count; index++)
        free(copies[index]);
    free(copies);
    errno = error_number;
    return -1;
}
}

void bx_fetch_config_free(struct bx_fetch_config* cfg) {
    if (!cfg)
        return;

    free(cfg->logging.log_file);
    free(cfg->logging.rejected_log);
    free(cfg->input.input_file);
    free(cfg->input.base_url);

    free(cfg->download.retry_on_http_error);
    free(cfg->download.output_document);
    free(cfg->download.dns_servers);
    free(cfg->download.bind_dns_address);
    free(cfg->download.bind_address);
    free(cfg->download.restrict_file_names);
    free(cfg->download.prefer_family);
    free(cfg->download.user);
    free(cfg->download.password);

    free(cfg->dirs.directory_prefix);

    free(cfg->http.http_user);
    free(cfg->http.http_password);
    free(cfg->http.default_page);
    for (int i = 0; i < cfg->http.header_count; i++) {
        free(cfg->http.headers[i]);
    }
    free(cfg->http.headers);
    free(cfg->http.redirect_method);
    free(cfg->http.proxy_user);
    free(cfg->http.proxy_password);
    free(cfg->http.referer);
    free(cfg->http.user_agent);
    free(cfg->http.load_cookies);
    free(cfg->http.save_cookies);
    free(cfg->http.post_data);
    free(cfg->http.post_file);
    free(cfg->http.method);

    free(cfg->https.certificate);
    free(cfg->https.private_key);
    free(cfg->https.ca_certificate);
    free(cfg->https.ca_directory);
    free(cfg->https.pinnedpubkey);
    free(cfg->https.hsts_file);

    free(cfg->ftp.ftp_user);
    free(cfg->ftp.ftp_password);

    free(cfg->recursive.accept_list);
    free(cfg->recursive.reject_list);
    free(cfg->recursive.accept_regex);
    free(cfg->recursive.regex_type);
    free(cfg->recursive.domains);
    free(cfg->recursive.exclude_domains);
    free(cfg->recursive.include_directories);
    free(cfg->recursive.exclude_directories);

    if (cfg->input.urls) {
        for (int i = 0; i < cfg->input.url_count; i++) {
            free(cfg->input.urls[i]);
        }
    }
    free(cfg->input.urls);

    free(cfg);
}
