#include "tls.h"

#include <errno.h>
#include <stdlib.h>

struct tls {
    int unused;
};

struct tls_config {
    int unused;
};

static const char tls_disabled_error[] = "TLS/SSL support is disabled in this build";

static int tls_disabled_fail(void) {
    errno = ENOTSUP;
    return -1;
}

int tls_init(void) {
    return 0;
}

struct tls_config* tls_config_new(void) {
    return calloc(1, sizeof(struct tls_config));
}

void tls_config_free(struct tls_config* config) {
    free(config);
}

const char* tls_config_error(struct tls_config* config) {
    (void)config;
    return tls_disabled_error;
}

int tls_config_set_ca_file(struct tls_config* config, const char* ca_file) {
    (void)config;
    (void)ca_file;
    return tls_disabled_fail();
}

int tls_config_set_ca_path(struct tls_config* config, const char* ca_path) {
    (void)config;
    (void)ca_path;
    return tls_disabled_fail();
}

int tls_config_set_cert_file(struct tls_config* config, const char* cert_file) {
    (void)config;
    (void)cert_file;
    return tls_disabled_fail();
}

int tls_config_set_cert_mem(struct tls_config* config, const uint8_t* cert, size_t len) {
    (void)config;
    (void)cert;
    (void)len;
    return tls_disabled_fail();
}

int tls_config_set_ciphers(struct tls_config* config, const char* ciphers) {
    (void)config;
    (void)ciphers;
    return tls_disabled_fail();
}

int tls_config_set_dheparams(struct tls_config* config, const char* params) {
    (void)config;
    (void)params;
    return tls_disabled_fail();
}

int tls_config_set_ecdhecurve(struct tls_config* config, const char* curve) {
    (void)config;
    (void)curve;
    return tls_disabled_fail();
}

int tls_config_set_key_file(struct tls_config* config, const char* key_file) {
    (void)config;
    (void)key_file;
    return tls_disabled_fail();
}

int tls_config_set_key_mem(struct tls_config* config, const uint8_t* key, size_t len) {
    (void)config;
    (void)key;
    (void)len;
    return tls_disabled_fail();
}

int tls_config_set_protocols(struct tls_config* config, uint32_t protocols) {
    (void)config;
    (void)protocols;
    return tls_disabled_fail();
}

int tls_config_set_verify_depth(struct tls_config* config, int verify_depth) {
    (void)config;
    (void)verify_depth;
    return tls_disabled_fail();
}

int tls_config_set_alpn(struct tls_config* config, const char* alpn) {
    (void)config;
    (void)alpn;
    return tls_disabled_fail();
}

int tls_config_set_dgram(struct tls_config* config, int dgram) {
    (void)config;
    (void)dgram;
    return tls_disabled_fail();
}

int tls_config_set_ocsp_staple_file(struct tls_config* config, const char* file) {
    (void)config;
    (void)file;
    return tls_disabled_fail();
}

void tls_config_prefer_ciphers_client(struct tls_config* config) {
    (void)config;
}

void tls_config_prefer_ciphers_server(struct tls_config* config) {
    (void)config;
}

void tls_config_insecure_noverifycert(struct tls_config* config) {
    (void)config;
}

void tls_config_insecure_noverifyname(struct tls_config* config) {
    (void)config;
}

void tls_config_insecure_noverifytime(struct tls_config* config) {
    (void)config;
}

void tls_config_verify_client(struct tls_config* config) {
    (void)config;
}

void tls_config_verify_client_optional(struct tls_config* config) {
    (void)config;
}

void tls_config_ocsp_require_stapling(struct tls_config* config) {
    (void)config;
}

int tls_config_parse_protocols(uint32_t* protocols, const char* protostr) {
    (void)protostr;
    if (protocols != NULL)
        *protocols = TLS_PROTOCOLS_DEFAULT;
    return tls_disabled_fail();
}

struct tls* tls_client(void) {
    return calloc(1, sizeof(struct tls));
}

struct tls* tls_server(void) {
    return calloc(1, sizeof(struct tls));
}

int tls_configure(struct tls* ctx, struct tls_config* config) {
    (void)ctx;
    (void)config;
    return tls_disabled_fail();
}

void tls_reset(struct tls* ctx) {
    (void)ctx;
}

void tls_free(struct tls* ctx) {
    free(ctx);
}

int tls_accept_socket(struct tls* ctx, struct tls** cctx, int socket_fd) {
    (void)ctx;
    (void)socket_fd;
    if (cctx != NULL)
        *cctx = NULL;
    return tls_disabled_fail();
}

int tls_connect_socket(struct tls* ctx, int socket_fd, const char* servername) {
    (void)ctx;
    (void)socket_fd;
    (void)servername;
    return tls_disabled_fail();
}

int tls_handshake(struct tls* ctx) {
    (void)ctx;
    return tls_disabled_fail();
}

ssize_t tls_read(struct tls* ctx, void* buf, size_t buflen) {
    (void)ctx;
    (void)buf;
    (void)buflen;
    errno = ENOTSUP;
    return -1;
}

ssize_t tls_write(struct tls* ctx, const void* buf, size_t buflen) {
    (void)ctx;
    (void)buf;
    (void)buflen;
    errno = ENOTSUP;
    return -1;
}

int tls_close(struct tls* ctx) {
    (void)ctx;
    return tls_disabled_fail();
}

const char* tls_error(struct tls* ctx) {
    (void)ctx;
    return tls_disabled_error;
}

int tls_peer_cert_provided(struct tls* ctx) {
    (void)ctx;
    return 0;
}

int tls_peer_cert_contains_name(struct tls* ctx, const char* name) {
    (void)ctx;
    (void)name;
    return 0;
}

const char* tls_peer_cert_hash(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_peer_cert_issuer(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_peer_cert_subject(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

time_t tls_peer_cert_notbefore(struct tls* ctx) {
    (void)ctx;
    return (time_t)-1;
}

time_t tls_peer_cert_notafter(struct tls* ctx) {
    (void)ctx;
    return (time_t)-1;
}

const uint8_t* tls_peer_cert_chain_pem(struct tls* ctx, size_t* len) {
    (void)ctx;
    if (len != NULL)
        *len = 0;
    return NULL;
}

const char* tls_conn_version(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_conn_cipher(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_conn_alpn_selected(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_conn_servername(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

const char* tls_peer_ocsp_url(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

int tls_peer_ocsp_response_status(struct tls* ctx) {
    (void)ctx;
    return -1;
}

const char* tls_peer_ocsp_result(struct tls* ctx) {
    (void)ctx;
    return NULL;
}

int tls_peer_ocsp_cert_status(struct tls* ctx) {
    (void)ctx;
    return -1;
}

int tls_peer_ocsp_crl_reason(struct tls* ctx) {
    (void)ctx;
    return -1;
}

time_t tls_peer_ocsp_this_update(struct tls* ctx) {
    (void)ctx;
    return (time_t)-1;
}

time_t tls_peer_ocsp_next_update(struct tls* ctx) {
    (void)ctx;
    return (time_t)-1;
}

time_t tls_peer_ocsp_revocation_time(struct tls* ctx) {
    (void)ctx;
    return (time_t)-1;
}

uint8_t* tls_load_file(const char* file, size_t* len, char* password) {
    (void)file;
    if (len != NULL)
        *len = 0;
    (void)password;
    errno = ENOTSUP;
    return NULL;
}

void tls_unload_file(uint8_t* buf, size_t len) {
    (void)len;
    free(buf);
}

const char* tls_default_ca_cert_file(void) {
    return NULL;
}
