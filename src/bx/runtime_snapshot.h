#ifndef BX_RUNTIME_SNAPSHOT_H
#define BX_RUNTIME_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>

#include "bx/applet_metadata.h"
#include "bx/applet_profile.h"
#include "dispatch/dispatch.h"

enum bx_runtime_config_policy {
    BX_RUNTIME_CONFIG_DEFAULT = 0,
    BX_RUNTIME_CONFIG_DISABLED,
    BX_RUNTIME_CONFIG_EXPLICIT,
};

enum bx_runtime_color_policy {
    BX_RUNTIME_COLOR_AUTO = 0,
    BX_RUNTIME_COLOR_EXPLICIT,
    BX_RUNTIME_COLOR_DISABLED,
};

enum bx_runtime_terminal_policy {
    BX_RUNTIME_TERMINAL_DEFAULT = 0,
    BX_RUNTIME_TERMINAL_COLOR_DISABLED,
};

enum bx_runtime_sandbox_policy {
    BX_RUNTIME_SANDBOX_DEFAULT = 0,
    BX_RUNTIME_SANDBOX_EXPLICIT,
};

enum bx_runtime_environment_policy {
    BX_RUNTIME_ENV_DEFAULT = 0,
    BX_RUNTIME_ENV_PROFILE,
};

struct bx_runtime_snapshot_spec {
    const struct bx_applet_profile *profile;
    const char *profile_source;
    bool json_requested;
    bool trace_requested;
    enum bx_runtime_config_policy config_policy;
    const char *config_path;
    enum bx_runtime_color_policy color_policy;
    const char *color_value;
    enum bx_runtime_terminal_policy terminal_policy;
    enum bx_runtime_sandbox_policy sandbox_policy;
    const char *sandbox_value;
    enum bx_runtime_environment_policy environment_policy;
};

/*
 * Opaque immutable dispatch/runtime policy snapshot.
 *
 * Mutable parsing state is converted to this object before applet dispatch.
 * It captures config policy, applet registry bounds, color policy, terminal
 * policy, sandbox policy, environment policy, and applet profile selection.
 * Consumers observe only const accessors; ownership stays with the coordinator
 * until dispatch returns.
 */
struct bx_runtime_snapshot;

struct bx_runtime_snapshot *bx_runtime_snapshot_create(
    const struct bx_runtime_snapshot_spec *spec);
void bx_runtime_snapshot_destroy(struct bx_runtime_snapshot *snapshot);

const struct bx_applet_profile *bx_runtime_snapshot_profile(
    const struct bx_runtime_snapshot *snapshot);
const char *bx_runtime_snapshot_profile_source(
    const struct bx_runtime_snapshot *snapshot);
bool bx_runtime_snapshot_json_requested(const struct bx_runtime_snapshot *snapshot);
bool bx_runtime_snapshot_trace_requested(const struct bx_runtime_snapshot *snapshot);
enum bx_runtime_config_policy bx_runtime_snapshot_config_policy(
    const struct bx_runtime_snapshot *snapshot);
const char *bx_runtime_snapshot_config_path(const struct bx_runtime_snapshot *snapshot);
enum bx_runtime_color_policy bx_runtime_snapshot_color_policy(
    const struct bx_runtime_snapshot *snapshot);
const char *bx_runtime_snapshot_color_value(const struct bx_runtime_snapshot *snapshot);
enum bx_runtime_terminal_policy bx_runtime_snapshot_terminal_policy(
    const struct bx_runtime_snapshot *snapshot);
enum bx_runtime_sandbox_policy bx_runtime_snapshot_sandbox_policy(
    const struct bx_runtime_snapshot *snapshot);
const char *bx_runtime_snapshot_sandbox_value(const struct bx_runtime_snapshot *snapshot);
enum bx_runtime_environment_policy bx_runtime_snapshot_environment_policy(
    const struct bx_runtime_snapshot *snapshot);

size_t bx_runtime_snapshot_applet_count(const struct bx_runtime_snapshot *snapshot);
const struct bx_applet *bx_runtime_snapshot_applet_at(
    const struct bx_runtime_snapshot *snapshot,
    size_t index);
size_t bx_runtime_snapshot_applet_metadata_count(
    const struct bx_runtime_snapshot *snapshot);
const struct bx_applet_metadata *bx_runtime_snapshot_applet_metadata_at(
    const struct bx_runtime_snapshot *snapshot,
    size_t index);

#endif /* BX_RUNTIME_SNAPSHOT_H */
