#ifndef BX_COMMON_ID_PARSE_H
#define BX_COMMON_ID_PARSE_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include "bx/diag.h"

struct bx_id_user {
    uid_t uid;
    bool symbolic;
    gid_t login_group;
};

struct bx_id_owner_group {
    bool owner_set;
    bool group_set;
    uid_t owner;
    gid_t group;
};

bool bx_id_parse_numeric(const char* text, uintmax_t max_value, uintmax_t* value_out);
bool bx_id_parse_user(const char* text, struct bx_id_user* user_out, struct bx_diag_ctx* diag);
bool bx_id_parse_owner(const char* text, uid_t* owner_out, struct bx_diag_ctx* diag);
bool bx_id_parse_group(const char* text, gid_t* group_out, struct bx_diag_ctx* diag);
bool bx_id_parse_owner_group(const char* text, struct bx_id_owner_group* parsed, struct bx_diag_ctx* diag);
bool bx_id_lookup_user(const char* text, uid_t* uid_out);
bool bx_id_lookup_group(const char* text, gid_t* gid_out);
bool bx_id_uid_exists(uid_t uid);
bool bx_id_gid_exists(gid_t gid);
const char* bx_id_user_name(uid_t uid, char numeric_buffer[32]);
const char* bx_id_group_name(gid_t gid, char numeric_buffer[32]);

#endif /* BX_COMMON_ID_PARSE_H */
