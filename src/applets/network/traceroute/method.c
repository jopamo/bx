#include <strings.h>
#include <string.h>
#include "traceroute.h"

extern const struct bx_traceroute_method bx_traceroute_method_default;
extern const struct bx_traceroute_method bx_traceroute_method_udp;
extern const struct bx_traceroute_method bx_traceroute_method_udplite;
extern const struct bx_traceroute_method bx_traceroute_method_icmp;
extern const struct bx_traceroute_method bx_traceroute_method_tcp;
extern const struct bx_traceroute_method bx_traceroute_method_tcpconn;
extern const struct bx_traceroute_method bx_traceroute_method_dccp;
extern const struct bx_traceroute_method bx_traceroute_method_raw;

static const struct bx_traceroute_method* const methods[] = {
    &bx_traceroute_method_default,
    &bx_traceroute_method_udp,
    &bx_traceroute_method_udplite,
    &bx_traceroute_method_icmp,
    &bx_traceroute_method_tcp,
    &bx_traceroute_method_tcpconn,
    &bx_traceroute_method_dccp,
    &bx_traceroute_method_raw,
};

const struct bx_traceroute_method* bx_traceroute_method_find(const char* name) {
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); i++) {
        if (!strcasecmp(methods[i]->name, name))
            return methods[i];
    }
    return NULL;
}

const struct bx_traceroute_method* bx_traceroute_method_by_id(enum bx_traceroute_method_id id) {
    for (size_t i = 0; i < sizeof(methods) / sizeof(*methods); i++) {
        if (methods[i]->id == id)
            return methods[i];
    }
    return NULL;
}

bool bx_traceroute_option_is(const char* option, const char* name, bool abbrev, const char** value) {
    const char* eq = strchr(option, '=');
    size_t len = eq ? (size_t)(eq - option) : strlen(option);
    if (len != strlen(name) && (!abbrev || len < 2 || len > strlen(name)))
        return false;
    if (strncmp(option, name, len))
        return false;
    *value = eq ? eq + 1 : NULL;
    return true;
}
