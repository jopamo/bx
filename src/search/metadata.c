#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "metadata.h"

bool bx_walk_numeric_match(unsigned long long actual, long long expected, int cmp) {
    unsigned long long want = (unsigned long long)expected;
    if (cmp > 0)
        return actual > want;
    if (cmp < 0)
        return actual < want;
    return actual == want;
}

bool bx_walk_mode_matches_perm(mode_t mode, mode_t bits, int kind) {
    mode_t actual = mode & 07777u;
    switch (kind) {
    case 0:
        return actual == bits;
    case 1:
        return (actual & bits) == bits;
    case 2:
        return bits == 0 ? true : (actual & bits) != 0;
    default:
        return false;
    }
}

bool bx_walk_size_matches(off_t size, long long expected, int cmp, unsigned long long unit) {
    unsigned long long bytes = size < 0 ? 0 : (unsigned long long)size;
    unsigned long long quanta = unit == 0 ? 0 : (bytes + unit - 1) / unit;
    return bx_walk_numeric_match(quanta, expected, cmp);
}

bool bx_walk_type_filter_is_valid(char type_filter, bool allow_extended) {
    switch (type_filter) {
    case 'f':
    case 'd':
    case 'l':
    case 'p':
    case 's':
    case 'b':
    case 'c':
        return true;
    case 'x':
    case 'e':
        return allow_extended;
    default:
        return false;
    }
}

bool bx_walk_parse_named_type_filter(const char *text, char *type_filter) {
    if (!text || !type_filter || *text == '\0')
        return false;

    if (strcmp(text, "f") == 0 || strcmp(text, "file") == 0) {
        *type_filter = 'f';
    } else if (strcmp(text, "d") == 0 || strcmp(text, "directory") == 0 || strcmp(text, "dir") == 0) {
        *type_filter = 'd';
    } else if (strcmp(text, "l") == 0 || strcmp(text, "symlink") == 0 || strcmp(text, "link") == 0) {
        *type_filter = 'l';
    } else if (strcmp(text, "x") == 0 || strcmp(text, "executable") == 0) {
        *type_filter = 'x';
    } else if (strcmp(text, "e") == 0 || strcmp(text, "empty") == 0) {
        *type_filter = 'e';
    } else if (strcmp(text, "p") == 0 || strcmp(text, "pipe") == 0) {
        *type_filter = 'p';
    } else if (strcmp(text, "s") == 0 || strcmp(text, "socket") == 0) {
        *type_filter = 's';
    } else if (strcmp(text, "b") == 0 || strcmp(text, "block") == 0 || strcmp(text, "block-device") == 0) {
        *type_filter = 'b';
    } else if (strcmp(text, "c") == 0 || strcmp(text, "char") == 0 || strcmp(text, "character-device") == 0) {
        *type_filter = 'c';
    } else {
        return false;
    }

    return true;
}

bool bx_walk_parse_unsigned_id(const char *text, unsigned long long *value) {
    if (!text || *text == '\0')
        return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!isdigit(*p))
            return false;
    }

    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return false;

    *value = v;
    return true;
}

bool bx_walk_resolve_user(const char *text, uid_t *value) {
    if (!text || !value)
        return false;

    struct passwd *pw = getpwnam(text);
    if (pw) {
        *value = pw->pw_uid;
        return true;
    }

    unsigned long long numeric = 0;
    if (!bx_walk_parse_unsigned_id(text, &numeric))
        return false;

    *value = (uid_t)numeric;
    return true;
}

bool bx_walk_resolve_group(const char *text, gid_t *value) {
    if (!text || !value)
        return false;

    struct group *gr = getgrnam(text);
    if (gr) {
        *value = gr->gr_gid;
        return true;
    }

    unsigned long long numeric = 0;
    if (!bx_walk_parse_unsigned_id(text, &numeric))
        return false;

    *value = (gid_t)numeric;
    return true;
}

bool bx_walk_uid_has_passwd(uid_t uid) {
    return getpwuid(uid) != NULL;
}

bool bx_walk_gid_has_group(gid_t gid) {
    return getgrgid(gid) != NULL;
}

bool bx_walk_entry_is_empty(struct walk_entry *entry) {
    if (!entry)
        return false;

    if (entry->is_dir) {
        DIR *dir = opendir(entry->path);
        if (!dir)
            return false;

        bool empty = true;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
                empty = false;
                break;
            }
        }
        closedir(dir);
        return empty;
    }

    if (!walk_entry_load_metadata(entry))
        return false;
    if (!S_ISREG(entry->mode))
        return false;
    return entry->size == 0;
}

bool bx_walk_entry_matches_type(struct walk_entry *entry, char type_filter) {
    if (!entry)
        return false;

    switch (type_filter) {
    case 'f':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISREG(entry->mode);
    case 'd':
        return entry->is_dir;
    case 'l':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISLNK(entry->mode);
    case 'x':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISREG(entry->mode) && access(entry->path, X_OK) == 0;
    case 'e':
        return bx_walk_entry_is_empty(entry);
    case 'p':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISFIFO(entry->mode);
    case 's':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISSOCK(entry->mode);
    case 'b':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISBLK(entry->mode);
    case 'c':
        if (!walk_entry_load_metadata(entry))
            return false;
        return S_ISCHR(entry->mode);
    default:
        return false;
    }
}
