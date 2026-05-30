#ifndef BX_APPLET_METADATA_H
#define BX_APPLET_METADATA_H

#include <stdbool.h>
#include <stddef.h>

struct bx_applet_metadata {
    const char* name;
    bool boot_critical;
    const char* const* capabilities;
    size_t capability_count;
    const char* const* aliases;
    size_t alias_count;
    const char* const* security_risk_labels;
    size_t security_risk_label_count;
    const char* const* performance_risk_labels;
    size_t performance_risk_label_count;
    const char* const* audit_levels;
    size_t audit_level_count;
};

size_t bx_applet_metadata_count(void);
const struct bx_applet_metadata* bx_applet_metadata_at(size_t index);
const struct bx_applet_metadata* bx_applet_metadata_find(const char* name);

#endif /* BX_APPLET_METADATA_H */
