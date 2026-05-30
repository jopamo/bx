#ifndef BX_APPLET_PROFILE_H
#define BX_APPLET_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include "dispatch/dispatch.h"

struct bx_applet_profile {
    const char* name;
    uint32_t allowed_capabilities;
};

const struct bx_applet_profile* bx_applet_profile_default(void);
const struct bx_applet_profile* bx_applet_profile_find(const char* name);
size_t bx_applet_profile_count(void);
const struct bx_applet_profile* bx_applet_profile_at(size_t index);
uint32_t bx_applet_profile_denied_capabilities(
    const struct bx_applet_profile* profile,
    const struct bx_applet* applet
);
void bx_applet_capabilities_format(uint32_t capabilities, char* buffer, size_t buffer_size);

#endif /* BX_APPLET_PROFILE_H */
