#ifndef BX_FETCH_CONFIG_H
#define BX_FETCH_CONFIG_H

/* BX_FETCH_HEADER_OWNER: runtime */
/* BX_FETCH_HEADER_CONSUMERS: runtime, entry, cli, core, policy, net, fs, store */

/*
 * Layering contract:
 * - This is the shared runtime configuration schema.
 * - Consumers may read fields but must not extend parsing behavior outside the
 *   CLI layer.
 *
 * Ownership and lifetime:
 * - EffectiveConfig and nested string/list fields are heap-owned by the config
 *   object itself.
 * - Callers allocate via bx_fetch_config_new() and release via bx_fetch_config_free().
 * - No field pointer remains valid after bx_fetch_config_free().
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool show_version;
    bool show_help;
    bool cli_options_provided;
} StartupConfig;

typedef enum {
    BX_FETCH_VERBOSITY_QUIET = 0,
    BX_FETCH_VERBOSITY_NORMAL,
    BX_FETCH_VERBOSITY_VERBOSE,
} MiraVerbosity;

typedef enum {
    BX_FETCH_LOG_FILE_NONE = 0,
    BX_FETCH_LOG_FILE_TRUNCATE,
    BX_FETCH_LOG_FILE_APPEND,
} MiraLogFileMode;

typedef struct {
    char* log_file;
    MiraLogFileMode log_file_mode;
    MiraVerbosity verbosity;
    bool debug_trace;
    bool structured_errors;
    char* rejected_log;
} LoggingConfig;

typedef struct {
    char* input_file;
    bool force_html;
    char* base_url;
    char** urls;
    int url_count;
} UrlInputConfig;

typedef struct {
    int tries;
    bool retry_connrefused;
    char* retry_on_http_error;
    char* output_document;
    bool no_clobber;
    bool continue_download;
    bool show_progress;
    bool timestamping;
    bool no_if_modified_since;
    bool no_use_server_timestamps;
    bool server_response;
    bool spider;
    bool dry_run;
    char* dns_servers;
    char* bind_dns_address;
    int dns_timeout;
    int connect_timeout;
    int read_timeout;
    int wait;
    int waitretry;
    bool random_wait;
    int max_threads;
    bool no_proxy;
    long quota;
    char* bind_address;
    int64_t limit_rate_bytes_per_sec;
    bool no_dns_cache;
    char* restrict_file_names;
    bool inet4_only;
#if HAVE_IPV6
    bool inet6_only;
#endif
    char* prefer_family;
    char* user;
    char* password;
    bool unlink;
    bool xattr;
} DownloadConfig;

typedef struct {
    bool no_directories;
    bool force_directories;
    bool no_host_directories;
    bool protocol_directories;
    char* directory_prefix;
    int cut_dirs;
    bool trust_server_names;
} DirectoryConfig;

typedef struct {
    char* http_user;
    char* http_password;
    char* default_page;
    bool adjust_extension;
    char** headers;
    int header_count;
    int max_redirect;
    char* redirect_method;
    bool paranoid;
    char* proxy_user;
    char* proxy_password;
    char* referer;
    bool save_headers;
    char* user_agent;
    bool no_http_keep_alive;
    bool no_cookies;
    char* load_cookies;
    char* save_cookies;
    bool keep_session_cookies;
    char* post_data;
    char* post_file;
    char* method;
    bool content_disposition;
    bool auth_no_challenge;
} HttpConfig;

typedef struct {
    bool https_only;
    bool no_check_certificate;
    char* certificate;
    char* private_key;
    char* ca_certificate;
    char* ca_directory;
    char* pinnedpubkey;
    bool no_hsts;
    char* hsts_file;
} HttpsConfig;

typedef struct {
    bool recursive;
    int level;
    bool convert_links;
    bool convert_file_only;
    int backups;
    bool backup_converted;
    bool page_requisites;
    char* accept_list;
    char* reject_list;
    char* accept_regex;
    char* regex_type;
    char* domains;
    char* exclude_domains;
    bool span_hosts;
    bool relative;
    char* include_directories;
    char* exclude_directories;
    bool no_parent;
} RecursiveConfig;

typedef struct {
    char* ftp_user;
    char* ftp_password;
    bool no_passive_ftp;
} FtpConfig;

typedef struct {
    StartupConfig startup;
    LoggingConfig logging;
    DownloadConfig download;
    DirectoryConfig dirs;
    HttpConfig http;
    HttpsConfig https;
    RecursiveConfig recursive;
    FtpConfig ftp;
    UrlInputConfig input;
} EffectiveConfig;

/* Allocates a config with default values; caller owns and must free. */
EffectiveConfig* bx_fetch_config_new(void);
/* Frees all nested allocations and the config object itself; NULL-safe. */
void bx_fetch_config_free(EffectiveConfig* config);

#endif  // BX_FETCH_CONFIG_H
