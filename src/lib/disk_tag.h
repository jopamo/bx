#ifndef BX_DISK_TAG_H
#define BX_DISK_TAG_H

/* For a recognized tag, return its value (which may be empty). */
const char* bx_disk_tag_value(const char* spec);

/* Resolve a recognized /dev/disk tag to an allocated block-device path.
 * Returns 1 on success, 0 for an unrecognized spec, or -1 with errno set.
 * In the latter two cases *device is NULL. */
int bx_disk_tag_resolve(const char* spec, char** device);

#endif
