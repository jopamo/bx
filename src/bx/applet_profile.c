#include "bx/applet_profile.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct bx_capability_name {
    uint32_t bit;
    const char* name;
};

#define BX_APPLET_CAPABILITY_ALL ( \
    BX_APPLET_CAP_FILESYSTEM_READ | \
    BX_APPLET_CAP_FILESYSTEM_WRITE | \
    BX_APPLET_CAP_RECURSIVE_TRAVERSAL | \
    BX_APPLET_CAP_CHILD_EXECUTION | \
    BX_APPLET_CAP_NETWORK_ACCESS | \
    BX_APPLET_CAP_TERMINAL_CONTROL | \
    BX_APPLET_CAP_PRIVILEGE_SENSITIVITY | \
    BX_APPLET_CAP_RAW_OUTPUT)

static const struct bx_capability_name bx_capability_names[] = {
    {BX_APPLET_CAP_FILESYSTEM_READ, "filesystem-read"},
    {BX_APPLET_CAP_FILESYSTEM_WRITE, "filesystem-write"},
    {BX_APPLET_CAP_RECURSIVE_TRAVERSAL, "recursive-traversal"},
    {BX_APPLET_CAP_CHILD_EXECUTION, "child-execution"},
    {BX_APPLET_CAP_NETWORK_ACCESS, "network-access"},
    {BX_APPLET_CAP_TERMINAL_CONTROL, "terminal-control"},
    {BX_APPLET_CAP_PRIVILEGE_SENSITIVITY, "privilege-sensitive"},
    {BX_APPLET_CAP_RAW_OUTPUT, "raw-output"},
};

static const struct bx_applet_profile bx_applet_profiles[] = {
    {"default", BX_APPLET_CAPABILITY_ALL},
    {"unrestricted", BX_APPLET_CAPABILITY_ALL},
    {
        "read-only",
        BX_APPLET_CAP_FILESYSTEM_READ |
        BX_APPLET_CAP_RECURSIVE_TRAVERSAL |
        BX_APPLET_CAP_TERMINAL_CONTROL |
        BX_APPLET_CAP_RAW_OUTPUT,
    },
    {"no-network", BX_APPLET_CAPABILITY_ALL & ~BX_APPLET_CAP_NETWORK_ACCESS},
    {"no-child", BX_APPLET_CAPABILITY_ALL & ~BX_APPLET_CAP_CHILD_EXECUTION},
};

const struct bx_applet_profile* bx_applet_profile_default(void) {
    return &bx_applet_profiles[0];
}

const struct bx_applet_profile* bx_applet_profile_find(const char* name) {
    if (name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < bx_applet_profile_count(); i++) {
        if (strcmp(bx_applet_profiles[i].name, name) == 0) {
            return &bx_applet_profiles[i];
        }
    }
    return NULL;
}

size_t bx_applet_profile_count(void) {
    return sizeof(bx_applet_profiles) / sizeof(bx_applet_profiles[0]);
}

const struct bx_applet_profile* bx_applet_profile_at(size_t index) {
    if (index >= bx_applet_profile_count()) {
        return NULL;
    }
    return &bx_applet_profiles[index];
}

uint32_t bx_applet_profile_denied_capabilities(
    const struct bx_applet_profile* profile,
    const struct bx_applet* applet
) {
    if (applet == NULL) {
        return BX_APPLET_CAPABILITY_ALL;
    }
    if (profile == NULL) {
        return applet->capabilities;
    }
    return applet->capabilities & ~profile->allowed_capabilities;
}

static void bx_append_capability_text(char* buffer, size_t buffer_size, size_t* offset, const char* text) {
    int written;
    size_t available;

    if (buffer_size == 0 || *offset >= buffer_size) {
        return;
    }

    available = buffer_size - *offset;
    written = snprintf(buffer + *offset, available, "%s", text);
    if (written < 0) {
        buffer[*offset] = '\0';
        return;
    }
    if ((size_t)written >= available) {
        *offset = buffer_size;
        return;
    }
    *offset += (size_t)written;
}

void bx_applet_capabilities_format(uint32_t capabilities, char* buffer, size_t buffer_size) {
    bool first = true;
    size_t offset = 0;
    uint32_t remaining = capabilities;

    if (buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    for (size_t i = 0; i < sizeof(bx_capability_names) / sizeof(bx_capability_names[0]); i++) {
        if ((capabilities & bx_capability_names[i].bit) == 0) {
            continue;
        }
        if (!first) {
            bx_append_capability_text(buffer, buffer_size, &offset, ",");
        }
        bx_append_capability_text(buffer, buffer_size, &offset, bx_capability_names[i].name);
        first = false;
        remaining &= ~bx_capability_names[i].bit;
    }

    if (remaining != 0) {
        char unknown[32];

        if (!first) {
            bx_append_capability_text(buffer, buffer_size, &offset, ",");
        }
        (void)snprintf(unknown, sizeof(unknown), "unknown(0x%08x)", remaining);
        bx_append_capability_text(buffer, buffer_size, &offset, unknown);
        first = false;
    }

    if (first) {
        bx_append_capability_text(buffer, buffer_size, &offset, "none");
    }
}
