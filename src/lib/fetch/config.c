#define _GNU_SOURCE
#include "lib/fetch/config.h"
#include <stdlib.h>
#include <string.h>

EffectiveConfig* config_new(void) {
    EffectiveConfig* cfg = calloc(1, sizeof(EffectiveConfig));
    if (!cfg)
        return NULL;

    // Set defaults
    cfg->download.tries = 20;
    cfg->logging.verbosity = MIRA_VERBOSITY_VERBOSE;
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
    cfg->http.user_agent = strdup("lib/fetch/0.1.0");
    cfg->download.prefer_family = strdup("none");
    if (!cfg->http.redirect_method || !cfg->http.user_agent || !cfg->download.prefer_family) {
        config_free(cfg);
        return NULL;
    }

    return cfg;
}

void config_free(EffectiveConfig* cfg) {
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
