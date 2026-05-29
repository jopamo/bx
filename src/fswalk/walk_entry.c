#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "walk_internal.h"

static enum bx_walk_counter bx_walk_metadata_reason_counter(enum bx_walk_metadata_reason reason) {
    switch (reason) {
    case BX_WALK_METADATA_REASON_TRAVERSAL_POLICY:
        return BX_WALK_COUNTER_STAT_REASON_TRAVERSAL_POLICY;
    case BX_WALK_METADATA_REASON_FILTER:
        return BX_WALK_COUNTER_STAT_REASON_METADATA_FILTER;
    case BX_WALK_METADATA_REASON_MAX_FILESIZE:
        return BX_WALK_COUNTER_STAT_REASON_MAX_FILESIZE;
    case BX_WALK_METADATA_REASON_MIN_FILESIZE:
        return BX_WALK_COUNTER_STAT_REASON_MIN_FILESIZE;
    case BX_WALK_METADATA_REASON_TYPE:
        return BX_WALK_COUNTER_STAT_REASON_TYPE;
    case BX_WALK_METADATA_REASON_SORT:
        return BX_WALK_COUNTER_STAT_REASON_SORT;
    case BX_WALK_METADATA_REASON_OUTPUT:
        return BX_WALK_COUNTER_STAT_REASON_METADATA_OUTPUT;
    }
    return BX_WALK_COUNTER_STAT_REASON_METADATA_FILTER;
}

void bx_walk_entry_fill_from_stat(struct bx_walk_entry *entry, const struct stat *st) {
    entry->is_dir = S_ISDIR(st->st_mode);
    entry->metadata_loaded = true;
    entry->metadata_tried = true;
    entry->dev = st->st_dev;
    entry->mode = st->st_mode;
    entry->inode = st->st_ino;
    entry->nlink = st->st_nlink;
    entry->uid = st->st_uid;
    entry->gid = st->st_gid;
    entry->size = st->st_size;
    entry->block_size = st->st_blksize;
    entry->atime = st->st_atim;
    entry->mtime = st->st_mtim;
    entry->ctime = st->st_ctim;
}

int bx_walk_entry_fill_from_dirent(struct bx_walk_entry *entry,
                                   char *path,
                                   const char *name,
                                   unsigned char d_type,
                                   int parent_dirfd,
                                   bool follow_symlinks,
                                   int depth,
                                   const struct bx_walk_counter_ops *counter_ops,
                                   bool *entry_was_symlink) {
    if (!entry || !entry_was_symlink)
        return EINVAL;

    memset(entry, 0, sizeof(*entry));
    entry->path = path;
    entry->d_type = d_type;
    entry->d_type_known = d_type != DT_UNKNOWN;
    entry->follow_metadata = follow_symlinks;
    entry->metadata_name = name;
    entry->metadata_dirfd = parent_dirfd;
    entry->metadata_dirfd_valid = parent_dirfd >= 0 && name != NULL;
    entry->depth = depth;
    entry->counter_ops = counter_ops;
    *entry_was_symlink = false;

    struct stat st;
    struct stat lst;

    if (!follow_symlinks) {
        if (d_type == DT_DIR) {
            entry->is_dir = true;
            return 0;
        }
        if (d_type == DT_LNK) {
            entry->is_symlink = true;
            return 0;
        }
        if (d_type != DT_UNKNOWN) {
            entry->is_dir = false;
            return 0;
        }

        if (parent_dirfd >= 0 && name) {
            bx_walk_note_stat_call_for_reason(counter_ops,
                                              BX_WALK_COUNTER_FSTATAT_CALLS,
                                              BX_WALK_COUNTER_STAT_REASON_UNKNOWN_DTYPE);
            if (fstatat(parent_dirfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                return errno;
        } else {
            bx_walk_note_stat_call_for_reason(counter_ops,
                                              BX_WALK_COUNTER_LSTAT_CALLS,
                                              BX_WALK_COUNTER_STAT_REASON_UNKNOWN_DTYPE);
            if (lstat(path, &st) != 0)
                return errno;
        }

        bx_walk_entry_fill_from_stat(entry, &st);
        entry->is_symlink = S_ISLNK(st.st_mode);
        return 0;
    }

    if (d_type == DT_DIR) {
        entry->is_dir = true;
        return 0;
    }
    if (d_type != DT_LNK && d_type != DT_UNKNOWN) {
        entry->is_dir = false;
        return 0;
    }

    enum bx_walk_counter reason_counter =
        d_type == DT_UNKNOWN
            ? BX_WALK_COUNTER_STAT_REASON_UNKNOWN_DTYPE
            : BX_WALK_COUNTER_STAT_REASON_SYMLINK_POLICY;
    if ((d_type == DT_LNK || d_type == DT_UNKNOWN) && parent_dirfd >= 0 && name) {
        bx_walk_note_stat_call_for_reason(counter_ops,
                                          BX_WALK_COUNTER_FSTATAT_CALLS,
                                          reason_counter);
        if (fstatat(parent_dirfd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0)
            return errno;
    } else {
        bx_walk_note_stat_call_for_reason(counter_ops,
                                          BX_WALK_COUNTER_LSTAT_CALLS,
                                          reason_counter);
        if (lstat(path, &lst) != 0)
            return errno;
    }

    *entry_was_symlink = S_ISLNK(lst.st_mode);
    entry->is_symlink = *entry_was_symlink;
    if (!*entry_was_symlink) {
        if (parent_dirfd >= 0 && name) {
            bx_walk_note_stat_call_for_reason(counter_ops,
                                              BX_WALK_COUNTER_FSTATAT_CALLS,
                                              reason_counter);
            if (fstatat(parent_dirfd, name, &st, 0) != 0)
                return errno;
        } else {
            bx_walk_note_stat_call_for_reason(counter_ops,
                                              BX_WALK_COUNTER_STAT_CALLS,
                                              reason_counter);
            if (stat(path, &st) != 0)
                return errno;
        }
        bx_walk_entry_fill_from_stat(entry, &st);
        return 0;
    }

    if (parent_dirfd >= 0 && name) {
        bx_walk_note_stat_call_for_reason(counter_ops,
                                          BX_WALK_COUNTER_FSTATAT_CALLS,
                                          BX_WALK_COUNTER_STAT_REASON_SYMLINK_POLICY);
        if (fstatat(parent_dirfd, name, &st, 0) == 0)
            bx_walk_entry_fill_from_stat(entry, &st);
        else
            bx_walk_entry_fill_from_stat(entry, &lst);
    } else {
        bx_walk_note_stat_call_for_reason(counter_ops,
                                          BX_WALK_COUNTER_STAT_CALLS,
                                          BX_WALK_COUNTER_STAT_REASON_SYMLINK_POLICY);
        if (stat(path, &st) == 0)
            bx_walk_entry_fill_from_stat(entry, &st);
        else
            bx_walk_entry_fill_from_stat(entry, &lst);
    }
    return 0;
}

bool bx_walk_entry_load_metadata_for(struct bx_walk_entry *entry,
                                     enum bx_walk_metadata_reason reason) {
    if (!entry)
        return false;
    if (entry->metadata_loaded)
        return true;
    if (entry->metadata_tried)
        return false;

    entry->metadata_tried = true;
    enum bx_walk_counter reason_counter = bx_walk_metadata_reason_counter(reason);

    struct stat st;
    int rc;
    if (entry->metadata_dirfd_valid) {
        int flags = entry->follow_metadata ? 0 : AT_SYMLINK_NOFOLLOW;
        bx_walk_note_stat_call_for_reason(entry->counter_ops,
                                          BX_WALK_COUNTER_FSTATAT_CALLS,
                                          reason_counter);
        rc = fstatat(entry->metadata_dirfd, entry->metadata_name, &st, flags);
    } else if (entry->follow_metadata) {
        bx_walk_note_stat_call_for_reason(entry->counter_ops,
                                          BX_WALK_COUNTER_STAT_CALLS,
                                          reason_counter);
        rc = stat(entry->path, &st);
    } else {
        bx_walk_note_stat_call_for_reason(entry->counter_ops,
                                          BX_WALK_COUNTER_LSTAT_CALLS,
                                          reason_counter);
        rc = lstat(entry->path, &st);
    }
    if (rc != 0)
        return false;

    bx_walk_entry_fill_from_stat(entry, &st);
    return true;
}
