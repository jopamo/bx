#include <inttypes.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
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
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno == ERANGE || end == text || end == NULL || end[0] != '\0') {
        return false;
    }
    if (value > max_value) {
        return false;
    }

    *value_out = value;
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

bool bx_id_lookup_user(const char* text, uid_t* uid_out) {
    if (text == NULL || uid_out == NULL) {
        return false;
    }

    struct bx_id_user user;
    struct bx_diag_ctx ignored_diag = {.progname = "", .exit_status = 0};
    if (!bx_id_parse_user(text, &user, &ignored_diag)) {
        return false;
    }

    *uid_out = user.uid;
    return true;
}

bool bx_id_lookup_group(const char* text, gid_t* gid_out) {
    if (text == NULL || gid_out == NULL) {
        return false;
    }

    struct bx_diag_ctx ignored_diag = {.progname = "", .exit_status = 0};
    if (!bx_id_parse_group(text, gid_out, &ignored_diag)) {
        return false;
    }

    return true;
}

bool bx_id_uid_exists(uid_t uid) {
    return getpwuid(uid) != NULL;
}

bool bx_id_gid_exists(gid_t gid) {
    return getgrgid(gid) != NULL;
}

const char* bx_id_user_name(uid_t uid, char numeric_buffer[32]) {
    struct passwd* passwd_entry = getpwuid(uid);
    if (passwd_entry != NULL && passwd_entry->pw_name != NULL && passwd_entry->pw_name[0] != '\0') {
        return passwd_entry->pw_name;
    }

    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)uid);
    return numeric_buffer;
}

const char* bx_id_group_name(gid_t gid, char numeric_buffer[32]) {
    struct group* group_entry = getgrgid(gid);
    if (group_entry != NULL && group_entry->gr_name != NULL && group_entry->gr_name[0] != '\0') {
        return group_entry->gr_name;
    }

    snprintf(numeric_buffer, 32, "%" PRIuMAX, (uintmax_t)gid);
    return numeric_buffer;
}
