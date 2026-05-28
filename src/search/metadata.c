#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "lib/id_parse.h"
#include "metadata.h"

static bool bx_walk_entry_mode_matches(struct bx_walk_entry *entry, bool (*predicate)(mode_t)) {
    if (!entry || !predicate)
        return false;

    if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_FILTER))
        return false;

    return predicate(entry->mode);
}

static bool bx_walk_mode_is_regular(mode_t mode) {
    return S_ISREG(mode);
}

static bool bx_walk_mode_is_symlink(mode_t mode) {
    return S_ISLNK(mode);
}

static bool bx_walk_mode_is_fifo(mode_t mode) {
    return S_ISFIFO(mode);
}

static bool bx_walk_mode_is_socket(mode_t mode) {
    return S_ISSOCK(mode);
}

static bool bx_walk_mode_is_block(mode_t mode) {
    return S_ISBLK(mode);
}

static bool bx_walk_mode_is_char(mode_t mode) {
    return S_ISCHR(mode);
}

static bool bx_walk_entry_matches_regular_type(struct bx_walk_entry *entry) {
    if (!entry)
        return false;
    if (entry->d_type_known)
        return entry->d_type == DT_REG;
    return bx_walk_entry_mode_matches(entry, bx_walk_mode_is_regular);
}

static bool bx_walk_entry_matches_known_dtype(struct bx_walk_entry *entry,
                                              unsigned char d_type,
                                              bool (*predicate)(mode_t)) {
    if (!entry)
        return false;
    if (entry->d_type_known)
        return entry->d_type == d_type;
    return bx_walk_entry_mode_matches(entry, predicate);
}

static bool bx_walk_entry_matches_directory_type(struct bx_walk_entry *entry) {
    if (!entry)
        return false;
    return entry->is_dir;
}

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
    if (!text || !value)
        return false;

    uintmax_t parsed = 0;
    if (!bx_id_parse_numeric(text, UINTMAX_MAX, &parsed))
        return false;

    *value = (unsigned long long)parsed;
    return true;
}

bool bx_walk_resolve_user(const char *text, uid_t *value) {
    return bx_id_lookup_user(text, value);
}

bool bx_walk_resolve_group(const char *text, gid_t *value) {
    return bx_id_lookup_group(text, value);
}

bool bx_walk_uid_has_passwd(uid_t uid) {
    return bx_id_uid_exists(uid);
}

bool bx_walk_gid_has_group(gid_t gid) {
    return bx_id_gid_exists(gid);
}

bool bx_walk_entry_is_empty(struct bx_walk_entry *entry) {
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

    if (!bx_walk_entry_mode_matches(entry, bx_walk_mode_is_regular))
        return false;
    return entry->size == 0;
}

bool bx_walk_entry_matches_type(struct bx_walk_entry *entry, char type_filter) {
    if (!entry)
        return false;

    switch (type_filter) {
    case 'f':
        return bx_walk_entry_matches_regular_type(entry);
    case 'd':
        return bx_walk_entry_matches_directory_type(entry);
    case 'l':
        return bx_walk_entry_matches_known_dtype(entry, DT_LNK, bx_walk_mode_is_symlink);
    case 'x':
        if (!bx_walk_entry_load_metadata_for(entry, BX_WALK_METADATA_REASON_FILTER))
            return false;
        return S_ISREG(entry->mode) && access(entry->path, X_OK) == 0;
    case 'e':
        return bx_walk_entry_is_empty(entry);
    case 'p':
        return bx_walk_entry_matches_known_dtype(entry, DT_FIFO, bx_walk_mode_is_fifo);
    case 's':
        return bx_walk_entry_matches_known_dtype(entry, DT_SOCK, bx_walk_mode_is_socket);
    case 'b':
        return bx_walk_entry_matches_known_dtype(entry, DT_BLK, bx_walk_mode_is_block);
    case 'c':
        return bx_walk_entry_matches_known_dtype(entry, DT_CHR, bx_walk_mode_is_char);
    default:
        return false;
    }
}
