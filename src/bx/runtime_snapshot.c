#include "bx/runtime_snapshot.h"

#include <stdlib.h>

#include "bx/libbx.h"

struct bx_runtime_snapshot {
    const struct bx_applet_profile *profile;
    char *profile_source;
    bool json_requested;
    bool trace_requested;
    enum bx_runtime_config_policy config_policy;
    char *config_path;
    enum bx_runtime_color_policy color_policy;
    char *color_value;
    enum bx_runtime_terminal_policy terminal_policy;
    enum bx_runtime_sandbox_policy sandbox_policy;
    char *sandbox_value;
    enum bx_runtime_environment_policy environment_policy;
    size_t applet_count;
    size_t applet_metadata_count;
};

static char *bx_runtime_snapshot_dup_optional(const char *value) {
    return value != NULL ? xstrdup(value) : NULL;
}

struct bx_runtime_snapshot *bx_runtime_snapshot_create(
    const struct bx_runtime_snapshot_spec *spec) {
    struct bx_runtime_snapshot *snapshot = xmalloc(sizeof(*snapshot));

    *snapshot = (struct bx_runtime_snapshot){
        .profile = spec != NULL && spec->profile != NULL
            ? spec->profile
            : bx_applet_profile_default(),
        .json_requested = spec != NULL && spec->json_requested,
        .trace_requested = spec != NULL && spec->trace_requested,
        .config_policy = spec != NULL
            ? spec->config_policy
            : BX_RUNTIME_CONFIG_DEFAULT,
        .color_policy = spec != NULL
            ? spec->color_policy
            : BX_RUNTIME_COLOR_AUTO,
        .terminal_policy = spec != NULL
            ? spec->terminal_policy
            : BX_RUNTIME_TERMINAL_DEFAULT,
        .sandbox_policy = spec != NULL
            ? spec->sandbox_policy
            : BX_RUNTIME_SANDBOX_DEFAULT,
        .environment_policy = spec != NULL
            ? spec->environment_policy
            : BX_RUNTIME_ENV_DEFAULT,
        .applet_count = bx_applet_count(),
        .applet_metadata_count = bx_applet_metadata_count(),
    };

    snapshot->profile_source = bx_runtime_snapshot_dup_optional(
        spec != NULL ? spec->profile_source : NULL);
    snapshot->config_path = bx_runtime_snapshot_dup_optional(
        spec != NULL ? spec->config_path : NULL);
    snapshot->color_value = bx_runtime_snapshot_dup_optional(
        spec != NULL ? spec->color_value : NULL);
    snapshot->sandbox_value = bx_runtime_snapshot_dup_optional(
        spec != NULL ? spec->sandbox_value : NULL);
    return snapshot;
}

void bx_runtime_snapshot_destroy(struct bx_runtime_snapshot *snapshot) {
    if (snapshot == NULL) {
        return;
    }
    free(snapshot->profile_source);
    free(snapshot->config_path);
    free(snapshot->color_value);
    free(snapshot->sandbox_value);
    free(snapshot);
}

const struct bx_applet_profile *bx_runtime_snapshot_profile(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->profile : bx_applet_profile_default();
}

const char *bx_runtime_snapshot_profile_source(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->profile_source : NULL;
}

bool bx_runtime_snapshot_json_requested(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL && snapshot->json_requested;
}

bool bx_runtime_snapshot_trace_requested(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL && snapshot->trace_requested;
}

enum bx_runtime_config_policy bx_runtime_snapshot_config_policy(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->config_policy : BX_RUNTIME_CONFIG_DEFAULT;
}

const char *bx_runtime_snapshot_config_path(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->config_path : NULL;
}

enum bx_runtime_color_policy bx_runtime_snapshot_color_policy(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->color_policy : BX_RUNTIME_COLOR_AUTO;
}

const char *bx_runtime_snapshot_color_value(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->color_value : NULL;
}

enum bx_runtime_terminal_policy bx_runtime_snapshot_terminal_policy(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL
        ? snapshot->terminal_policy
        : BX_RUNTIME_TERMINAL_DEFAULT;
}

enum bx_runtime_sandbox_policy bx_runtime_snapshot_sandbox_policy(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL
        ? snapshot->sandbox_policy
        : BX_RUNTIME_SANDBOX_DEFAULT;
}

const char *bx_runtime_snapshot_sandbox_value(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->sandbox_value : NULL;
}

enum bx_runtime_environment_policy bx_runtime_snapshot_environment_policy(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL
        ? snapshot->environment_policy
        : BX_RUNTIME_ENV_DEFAULT;
}

size_t bx_runtime_snapshot_applet_count(const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->applet_count : 0u;
}

const struct bx_applet *bx_runtime_snapshot_applet_at(
    const struct bx_runtime_snapshot *snapshot,
    size_t index) {
    if (snapshot == NULL || index >= snapshot->applet_count) {
        return NULL;
    }
    return bx_applet_at(index);
}

size_t bx_runtime_snapshot_applet_metadata_count(
    const struct bx_runtime_snapshot *snapshot) {
    return snapshot != NULL ? snapshot->applet_metadata_count : 0u;
}

const struct bx_applet_metadata *bx_runtime_snapshot_applet_metadata_at(
    const struct bx_runtime_snapshot *snapshot,
    size_t index) {
    if (snapshot == NULL || index >= snapshot->applet_metadata_count) {
        return NULL;
    }
    return bx_applet_metadata_at(index);
}
