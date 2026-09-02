#ifndef BX_APPLETS_SHELL_ASH_PRIVILEGE_H
#define BX_APPLETS_SHELL_ASH_PRIVILEGE_H

#include <stdbool.h>
#include <sys/types.h>

struct ash_credential_snapshot {
    uid_t real_user;
    uid_t effective_user;
    gid_t real_group;
    gid_t effective_group;
};

struct ash_privilege_plan {
    uid_t real_user;
    gid_t real_group;
    bool privileged;
    bool suppress_startup;
    bool reset_effective_credentials;
};

typedef int (*ash_set_user_id_fn)(uid_t user);
typedef int (*ash_set_group_id_fn)(gid_t group);

struct ash_credential_snapshot ash_credential_snapshot_current(void);
bool ash_privilege_plan_build(
    const struct ash_credential_snapshot* credentials,
    bool preserve_privileges,
    struct ash_privilege_plan* plan
);
bool ash_privilege_plan_valid(const struct ash_privilege_plan* plan);
bool ash_privilege_plan_apply_with(
    const struct ash_privilege_plan* plan,
    ash_set_group_id_fn set_group_id,
    ash_set_user_id_fn set_user_id
);
bool ash_privilege_plan_apply(const struct ash_privilege_plan* plan);

#endif /* BX_APPLETS_SHELL_ASH_PRIVILEGE_H */
