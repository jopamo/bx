#include <sys/stat.h>

#include "walk_internal.h"

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

bool bx_walk_entry_load_metadata(struct bx_walk_entry *entry) {
    if (!entry)
        return false;
    if (entry->metadata_loaded)
        return true;
    if (entry->metadata_tried)
        return false;

    entry->metadata_tried = true;

    struct stat st;
    int rc;
    if (entry->follow_metadata) {
        rc = stat(entry->path, &st);
    } else {
        bx_walk_note_counter(entry->counter_ops, BX_WALK_COUNTER_LSTAT_CALLS, 1u);
        rc = lstat(entry->path, &st);
    }
    if (rc != 0)
        return false;

    bx_walk_entry_fill_from_stat(entry, &st);
    return true;
}
