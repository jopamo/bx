#ifndef BX_LIB_SYSTEM_SYSLOG_CONFIG_H
#define BX_LIB_SYSTEM_SYSLOG_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include "lib/system/syslog_core.h"

void bx_syslog_config_init(struct bx_syslog_config *config);
void bx_syslog_config_destroy(struct bx_syslog_config *config);

bool bx_syslog_config_load(
    struct bx_syslog_config *candidate,
    const char *path,
    bool missing_is_ok,
    char *error,
    size_t error_capacity);

#endif
