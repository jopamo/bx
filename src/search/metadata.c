#define _GNU_SOURCE
#include <dirent.h>
#include <stdbool.h>
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
