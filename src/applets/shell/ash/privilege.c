#define _GNU_SOURCE

#include <errno.h>
#include <unistd.h>

#include "applets/shell/ash/privilege.h"

static int ash_reset_user_ids(uid_t user) {
    return setreuid(user, user);
}

static int ash_reset_group_ids(gid_t group) {
    return setregid(group, group);
}

struct ash_credential_snapshot ash_credential_snapshot_current(void) {
    return (struct ash_credential_snapshot){
        .real_user = getuid(),
        .effective_user = geteuid(),
        .real_group = getgid(),
        .effective_group = getegid(),
    };
}

bool ash_privilege_plan_valid(const struct ash_privilege_plan* plan) {
    if (plan == NULL) {
        return false;
    }
    if (plan->privileged) {
        return !plan->reset_effective_credentials;
    }
    return plan->suppress_startup ==
        plan->reset_effective_credentials;
}

bool ash_privilege_plan_build(
    const struct ash_credential_snapshot* credentials,
    bool preserve_privileges,
    struct ash_privilege_plan* plan
) {
    if (credentials == NULL || plan == NULL) {
        errno = EINVAL;
        return false;
    }
    bool credentials_differ =
        credentials->real_user != credentials->effective_user ||
        credentials->real_group != credentials->effective_group;
    struct ash_privilege_plan candidate = {
        .real_user = credentials->real_user,
        .real_group = credentials->real_group,
        .privileged = preserve_privileges,
        .suppress_startup = credentials_differ,
        .reset_effective_credentials =
            !preserve_privileges && credentials_differ,
    };
    if (!ash_privilege_plan_valid(&candidate)) {
        errno = EINVAL;
        return false;
    }
    *plan = candidate;
    return true;
}

bool ash_privilege_plan_apply_with(
    const struct ash_privilege_plan* plan,
    ash_set_group_id_fn set_group_id,
    ash_set_user_id_fn set_user_id
) {
    if (!ash_privilege_plan_valid(plan) ||
        (plan->reset_effective_credentials &&
         (set_group_id == NULL || set_user_id == NULL))) {
        errno = EINVAL;
        return false;
    }
    if (!plan->reset_effective_credentials) {
        return true;
    }
    /*
     * Drop group authority first: dropping user authority can remove the
     * ability to change the effective group. Both callbacks reset real and
     * effective IDs to the snapshotted real IDs; the platform wrappers use
     * setreuid/setregid so saved elevated credentials are cleared as well.
     */
    return set_group_id(plan->real_group) == 0 &&
        set_user_id(plan->real_user) == 0;
}

bool ash_privilege_plan_apply(const struct ash_privilege_plan* plan) {
    return ash_privilege_plan_apply_with(
        plan,
        ash_reset_group_ids,
        ash_reset_user_ids
    );
}
