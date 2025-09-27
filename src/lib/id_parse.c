#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>

#include "lib/id_parse.h"
#include "bx/libbx.h"

static char* bx_id_dup_slice(const char* text, size_t len) {
    char* copy = xmalloc(len + 1u);
    memcpy(copy, text, len);
    copy[len] = '\0';
    return copy;
}

bool bx_id_parse_numeric(const char* text, uintmax_t max_value, uintmax_t* value_out) {
    if (text == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }
    if ((uintmax_t)value > max_value) {
        return false;
    }

    *value_out = (uintmax_t)value;
    return true;
}

bool bx_id_parse_user(const char* text, struct bx_id_user* user_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_id_parse_numeric(text, (uintmax_t)((uid_t)-1), &numeric_id)) {
        user_out->uid = (uid_t)numeric_id;
        user_out->symbolic = false;
        user_out->login_group = 0;
        return true;
    }

    struct passwd* passwd_entry = getpwnam(text);
    if (passwd_entry != NULL) {
        user_out->uid = passwd_entry->pw_uid;
        user_out->symbolic = true;
        user_out->login_group = passwd_entry->pw_gid;
        return true;
    }

    bx_diag(diag, "invalid user '%s'", (text != NULL) ? text : "");
    return false;
}

bool bx_id_parse_owner(const char* text, uid_t* owner_out, struct bx_diag_ctx* diag) {
    struct bx_id_user user;
    if (!bx_id_parse_user(text, &user, diag)) {
        return false;
    }

    *owner_out = user.uid;
    return true;
}

bool bx_id_parse_group(const char* text, gid_t* group_out, struct bx_diag_ctx* diag) {
    uintmax_t numeric_id = 0;
    if (bx_id_parse_numeric(text, (uintmax_t)((gid_t)-1), &numeric_id)) {
        *group_out = (gid_t)numeric_id;
        return true;
    }

    struct group* group_entry = getgrnam(text);
    if (group_entry != NULL) {
        *group_out = group_entry->gr_gid;
        return true;
    }

    bx_diag(diag, "invalid group '%s'", (text != NULL) ? text : "");
    return false;
}

bool bx_id_parse_owner_group(const char* text, struct bx_id_owner_group* parsed, struct bx_diag_ctx* diag) {
    memset(parsed, 0, sizeof(*parsed));

    if (text == NULL) {
        bx_diag(diag, "missing operand");
        return false;
    }

    const char* separator = strchr(text, ':');
    if (separator == NULL) {
        if (text[0] != '\0') {
            struct bx_id_user owner;
            if (!bx_id_parse_user(text, &owner, diag)) {
                return false;
            }
            parsed->owner = owner.uid;
            parsed->owner_set = true;
        }
        return true;
    }

    size_t owner_len = (size_t)(separator - text);
    const char* group_text = separator + 1;
    bool owner_is_symbolic = false;
    gid_t owner_login_group = 0;

    if (owner_len > 0u) {
        char* owner_text = bx_id_dup_slice(text, owner_len);
        struct bx_id_user owner;
        bool ok = bx_id_parse_user(owner_text, &owner, diag);
        free(owner_text);
        if (!ok) {
            return false;
        }

        parsed->owner = owner.uid;
        parsed->owner_set = true;
        owner_is_symbolic = owner.symbolic;
        owner_login_group = owner.login_group;
    }

    if (group_text[0] != '\0') {
        if (!bx_id_parse_group(group_text, &parsed->group, diag)) {
            return false;
        }
        parsed->group_set = true;
    }
    else if (owner_is_symbolic) {
        parsed->group = owner_login_group;
        parsed->group_set = true;
    }

    return true;
}
