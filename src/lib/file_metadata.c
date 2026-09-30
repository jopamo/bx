#define _GNU_SOURCE
#include "lib/file_metadata.h"

#include <acl/libacl.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/xattr.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bx/libbx.h"

void bx_file_metadata_free(struct bx_file_metadata* metadata) {
    for (size_t i = 0; i < metadata->len; i++) {
        free(metadata->xattrs[i].name);
        free(metadata->xattrs[i].value);
    }
    free(metadata->xattrs);
    free(metadata->acl_access);
    free(metadata->acl_default);
    free(metadata->file_flags);
    memset(metadata, 0, sizeof(*metadata));
}

bool bx_file_metadata_set(struct bx_file_metadata* metadata, const char* name,
                          const void* value, size_t size) {
    if (!*name || strlen(name) > 255u || size > 65536u) {
        errno = EINVAL;
        return false;
    }
    size_t i;
    for (i = 0; i < metadata->len; i++) {
        if (strcmp(metadata->xattrs[i].name, name) == 0)
            break;
    }
    if (i == metadata->len) {
        metadata->xattrs = xrealloc(metadata->xattrs,
                                    (metadata->len + 1u) * sizeof(*metadata->xattrs));
        metadata->xattrs[i] = (struct bx_file_xattr){.name = xstrdup(name)};
        metadata->len++;
    }
    free(metadata->xattrs[i].value);
    metadata->xattrs[i].value = xmalloc(size ? size : 1u);
    if (size)
        memcpy(metadata->xattrs[i].value, value, size);
    metadata->xattrs[i].size = size;
    return true;
}

void bx_file_metadata_remove(struct bx_file_metadata* metadata, const char* name) {
    for (size_t i = 0; i < metadata->len; i++) {
        if (strcmp(metadata->xattrs[i].name, name) != 0)
            continue;
        free(metadata->xattrs[i].name);
        free(metadata->xattrs[i].value);
        memmove(&metadata->xattrs[i], &metadata->xattrs[i + 1u], (metadata->len - i - 1u) * sizeof(*metadata->xattrs));
        metadata->len--;
        return;
    }
}

void bx_file_metadata_overlay(struct bx_file_metadata* dest, const struct bx_file_metadata* source) {
    if (dest == source)
        return;
    for (size_t i = 0; i < source->len; i++) {
        const struct bx_file_xattr* attr = &source->xattrs[i];
        bx_file_metadata_set(dest, attr->name, attr->value, attr->size);
    }
    if (source->acl_access) {
        free(dest->acl_access);
        dest->acl_access = xstrdup(source->acl_access);
    }
    if (source->acl_default) {
        free(dest->acl_default);
        dest->acl_default = xstrdup(source->acl_default);
    }
    if (source->file_flags) {
        free(dest->file_flags);
        dest->file_flags = xstrdup(source->file_flags);
    }
    dest->restore_acls |= source->restore_acls;
}

void bx_file_metadata_copy(struct bx_file_metadata* dest, const struct bx_file_metadata* source) {
    if (dest == source)
        return;
    bx_file_metadata_free(dest);
    bx_file_metadata_overlay(dest, source);
}

size_t bx_file_metadata_value_bytes(const struct bx_file_metadata* metadata) {
    size_t total = 0u;
    for (size_t i = 0; i < metadata->len; i++) {
        size_t name = strlen(metadata->xattrs[i].name) + 1u;
        size_t value = metadata->xattrs[i].size;
        if (value > SIZE_MAX - name || total > SIZE_MAX - name - value)
            return SIZE_MAX;
        total += name + value;
    }
    const char* texts[] = {metadata->acl_access, metadata->acl_default, metadata->file_flags};
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]); i++) {
        if (texts[i]) {
            size_t length = strlen(texts[i]) + 1u;
            if (total > SIZE_MAX - length)
                return SIZE_MAX;
            total += length;
        }
    }
    return total;
}

bool bx_file_metadata_read_flags(int fd, unsigned int* flags, bool* applicable) {
    struct stat status;
    *flags = 0u;
    *applicable = false;
    if (fstat(fd, &status) != 0)
        return false;
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode))
        return true;
    *applicable = true;
    int value = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &value) != 0)
        return false;
    *flags = (unsigned int)value;
    return true;
}

bool bx_file_metadata_apply_flags(int fd, unsigned int set, unsigned int clear) {
    if (set & clear) {
        errno = EINVAL;
        return false;
    }
    if (!(set | clear))
        return true;
    unsigned int original;
    bool applicable;
    if (!bx_file_metadata_read_flags(fd, &original, &applicable))
        return false;
    if (!applicable) {
        errno = ENOTSUP;
        return false;
    }
    unsigned int wanted = (original & ~clear) | set;
    int value = (int)wanted;
    if (wanted != original && ioctl(fd, FS_IOC_SETFLAGS, &value) != 0)
        return false;
    unsigned int observed;
    if (!bx_file_metadata_read_flags(fd, &observed, &applicable))
        return false;
    if (((observed ^ wanted) & (set | clear)) != 0u) {
        errno = ENOTSUP;
        return false;
    }
    return true;
}

static ssize_t bx_metadata_get_xattr(int fd, const char* name, void* value, size_t size);

/* Decode Linux's version-2 ACL xattrs through the pinned-object interface.
 * ENODATA denotes a mode-derived access ACL or an empty default ACL, not an
 * unsupported interface. */
static acl_t bx_metadata_acl_fd(int fd, acl_type_t type) {
    unsigned char* data = xmalloc(65536u);
    ssize_t size = bx_metadata_get_xattr(fd, type == ACL_TYPE_DEFAULT ? "system.posix_acl_default" : "system.posix_acl_access", data, 65536u);
    acl_t acl = NULL;
    if (size < 0) {
        if (errno == ENODATA) {
            if (type == ACL_TYPE_DEFAULT)
                acl = acl_init(0);
            else {
                struct stat status;
                if (fstat(fd, &status) == 0)
                    acl = acl_from_mode(status.st_mode);
            }
        }
        goto out;
    }
    uint32_t version;
    if (size < 4 || (size - 4) % 8 != 0 || (type == ACL_TYPE_ACCESS && size == 4))
        goto invalid;
    memcpy(&version, data, sizeof(version));
    if (le32toh(version) != 2)
        goto invalid;
    acl = acl_init((int)((size - 4) / 8));
    if (!acl)
        goto out;
    for (ssize_t pos = 4; pos < size; pos += 8) {
        uint16_t tag, perms;
        uint32_t id;
        memcpy(&tag, data + pos, sizeof(tag));
        memcpy(&perms, data + pos + 2, sizeof(perms));
        memcpy(&id, data + pos + 4, sizeof(id));
        tag = le16toh(tag);
        perms = le16toh(perms);
        id = le32toh(id);
        if ((perms & ~7u) || (tag != ACL_USER && tag != ACL_GROUP && id != UINT32_MAX))
            goto invalid;
        acl_entry_t entry;
        acl_permset_t set;
        if (acl_create_entry(&acl, &entry) != 0 || acl_set_tag_type(entry, tag) != 0
            || acl_get_permset(entry, &set) != 0 || acl_clear_perms(set) != 0
            || ((perms & ACL_READ) && acl_add_perm(set, ACL_READ) != 0)
            || ((perms & ACL_WRITE) && acl_add_perm(set, ACL_WRITE) != 0)
            || ((perms & ACL_EXECUTE) && acl_add_perm(set, ACL_EXECUTE) != 0))
            goto fail;
        if (tag == ACL_USER) {
            uid_t uid = (uid_t)id;
            if (id == UINT32_MAX || (uint32_t)uid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &uid) != 0)
                goto fail;
        } else if (tag == ACL_GROUP) {
            gid_t gid = (gid_t)id;
            if (id == UINT32_MAX || (uint32_t)gid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &gid) != 0)
                goto fail;
        }
    }
    if (size > 4 && acl_valid(acl) != 0)
        goto fail;
    goto out;
invalid:
    errno = EINVAL;
fail: {
    int error = errno;
    if (acl)
        acl_free(acl);
    acl = NULL;
    errno = error;
}
out: {
    int error = errno;
    free(data);
    errno = error;
    return acl;
}
}

static bool bx_metadata_read_acl(char** text, int fd, acl_type_t type,
                                  bool numeric_ids) {
    acl_t acl = bx_metadata_acl_fd(fd, type);
    if (!acl)
        return false;
    char* value = acl_to_any_text(acl, NULL, ',', numeric_ids ? TEXT_NUMERIC_IDS : 0);
    acl_free(acl);
    if (!value)
        return false;
    *text = xstrdup(value);
    acl_free(value);
    return true;
}

/* Newer kernels can support empty-path xattr operations on O_PATH FDs even
 * where the older f*xattr interfaces reject them. Never retry by pathname. */
static ssize_t bx_metadata_list_xattrs(int fd, char* names, size_t size) {
    ssize_t rc = flistxattr(fd, names, size);
#ifdef SYS_listxattrat
    if (rc < 0 && errno == EBADF)
        rc = syscall(SYS_listxattrat, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, names, size);
#endif
    return rc;
}

static ssize_t bx_metadata_get_xattr(int fd, const char* name, void* value, size_t size) {
    ssize_t rc = fgetxattr(fd, name, value, size);
#ifdef SYS_getxattrat
    if (rc < 0 && errno == EBADF) {
        /* Linux xattr_args ABI, also aligned to eight bytes on 32-bit targets.
         * Keep this usable with libc headers that do not declare xattrat yet. */
        struct {
            _Alignas(8) uint64_t value;
            uint32_t size;
            uint32_t flags;
        } args = {.value = (uintptr_t)value, .size = (uint32_t)size};
        rc = syscall(SYS_getxattrat, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                       name, &args, sizeof(args));
    }
#endif
    return rc;
}

bool bx_file_metadata_read(struct bx_file_metadata* metadata, int fd,
                           bool symlink, bool directory, bool acls, bool numeric_ids,
                           bx_file_xattr_filter filter, const void* user) {
    if (fd < 0 && (filter || (acls && !symlink))) {
        errno = EBADF;
        return false;
    }
    if (acls && !symlink) {
        if (!bx_metadata_read_acl(&metadata->acl_access, fd, ACL_TYPE_ACCESS, numeric_ids)
            || (directory && !bx_metadata_read_acl(&metadata->acl_default, fd,
                                                    ACL_TYPE_DEFAULT, numeric_ids)))
            return false;
    }
    if (!filter)
        return true;
    /* Linux bounds the name list and each value at 64 KiB. Read each in one
     * syscall, avoiding a size-query/read race when another writer changes it. */
    char* names = xmalloc(65536u);
    unsigned char* value = xmalloc(65536u);
    ssize_t size = bx_metadata_list_xattrs(fd, names, 65536u);
    bool ok = size >= 0;
    for (ssize_t pos = 0; ok && pos < size;) {
        const char* name = names + pos;
        pos += (ssize_t)strlen(name) + 1;
        if (!filter(name, user))
            continue;
        ssize_t len = bx_metadata_get_xattr(fd, name, value, 65536u);
        if (len < 0) {
            ok = false;
            break;
        }
        ok = bx_file_metadata_set(metadata, name, value, (size_t)len);
    }
    int error = errno;
    free(value);
    free(names);
    /* EBADF here can mean a valid O_PATH descriptor whose xattr operations
     * the kernel does not support, rather than a closed descriptor. */
    if (!ok && error == EBADF) {
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0 && (flags & O_PATH))
            error = EOPNOTSUPP;
    }
    errno = error;
    return ok;
}

/* POSIX draft ACL text can carry a fourth, numeric-ID field (star). Linux
 * libacl accepts three fields; retain the named identity and discard extras. */
static acl_t bx_metadata_acl_from_text(const char* text) {
    char* copy = xstrdup(text);
    char* out = copy;
    unsigned int colons = 0;
    for (const char* in = text; *in; in++) {
        if (*in == ',' || *in == '\n') {
            colons = 0;
            *out++ = *in;
        }
        else if (*in == ':' && ++colons >= 3u) {
            continue;
        }
        else if (colons < 3u)
            *out++ = *in;
    }
    *out = '\0';
    acl_t acl = acl_from_text(copy);
    free(copy);
    if (acl && acl_valid(acl) != 0) {
        int error = errno;
        acl_free(acl);
        errno = error;
        return NULL;
    }
    return acl;
}

static int bx_metadata_set_xattr(int fd, const char* name, const void* value, size_t size) {
    int rc = fsetxattr(fd, name, value, size, 0);
#ifdef __NR_setxattrat
    if (rc < 0 && errno == EBADF) {
        struct {
            _Alignas(8) uint64_t value;
            uint32_t size;
            uint32_t flags;
        } args = {.value = (uintptr_t)value, .size = (uint32_t)size};
        rc = (int)syscall(__NR_setxattrat, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                           name, &args, sizeof(args));
    }
#endif
    return rc;
}

/* libacl has no default-ACL fd setter. Encode the same Linux xattr format
 * used by the fd reader; empty-path writes also support O_PATH references. */
static int bx_metadata_set_acl(int fd, acl_t acl, bool defaults, bool path_fd) {
    if (!defaults && !path_fd)
        return acl_set_fd(fd, acl);
    int count = acl_entries(acl);
    if (count < 0)
        return -1;
    if (count > (65536 - 4) / 8) {
        errno = E2BIG;
        return -1;
    }
    size_t size = 4u + (size_t)count * 8u;
    unsigned char* data = xmalloc(size);
    uint32_t version = htole32(2);
    memcpy(data, &version, sizeof(version));
    acl_entry_t entry;
    int rc = -1;
    for (int i = 0; i < count; i++) {
        acl_tag_t tag;
        acl_permset_t perms;
        if (acl_get_entry(acl, i ? ACL_NEXT_ENTRY : ACL_FIRST_ENTRY, &entry) != 1
            || acl_get_tag_type(entry, &tag) != 0 || acl_get_permset(entry, &perms) != 0)
            goto out;
        uint32_t id = UINT32_MAX;
        if (tag == ACL_USER || tag == ACL_GROUP) {
            void* qualifier = acl_get_qualifier(entry);
            if (!qualifier)
                goto out;
            id = tag == ACL_USER ? *(uid_t*)qualifier : *(gid_t*)qualifier;
            acl_free(qualifier);
        }
        uint16_t bits = 0;
        const acl_perm_t permissions[] = {ACL_READ, ACL_WRITE, ACL_EXECUTE};
        for (size_t j = 0; j < sizeof(permissions) / sizeof(permissions[0]); j++) {
            int present = acl_get_perm(perms, permissions[j]);
            if (present < 0)
                goto out;
            if (present)
                bits |= permissions[j];
        }
        uint16_t wire_tag = htole16(tag), wire_bits = htole16(bits);
        uint32_t wire_id = htole32(id);
        unsigned char* dest = data + 4u + (size_t)i * 8u;
        memcpy(dest, &wire_tag, sizeof(wire_tag));
        memcpy(dest + 2, &wire_bits, sizeof(wire_bits));
        memcpy(dest + 4, &wire_id, sizeof(wire_id));
    }
    rc = bx_metadata_set_xattr(fd,
        defaults ? "system.posix_acl_default" : "system.posix_acl_access", data, size);
out:
    {
        int error = errno;
        free(data);
        errno = error;
    }
    return rc;
}

static bool bx_metadata_delete_default_acl(int fd) {
    int rc = fremovexattr(fd, "system.posix_acl_default");
#ifdef __NR_removexattrat
    if (rc < 0 && errno == EBADF)
        rc = (int)syscall(__NR_removexattrat, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                           "system.posix_acl_default");
#endif
    return rc == 0 || errno == ENODATA;
}

static bool bx_metadata_apply_xattr(const struct bx_file_xattr* attr, int fd) {
    int rc = bx_metadata_set_xattr(fd, attr->name, attr->value, attr->size);
    return rc == 0;
}

bool bx_file_metadata_apply(const struct bx_file_metadata* metadata, int fd,
                            bool symlink, bool directory,
                            mode_t mode) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return false;
    bool path_fd = (flags & O_PATH) != 0;
    if (metadata->restore_acls && !symlink) {
        acl_t access = metadata->acl_access
            ? bx_metadata_acl_from_text(metadata->acl_access)
            : acl_from_mode(mode & 0777u);
        if (!access)
            return false;
        int rc = bx_metadata_set_acl(fd, access, false, path_fd);
        int error = errno;
        acl_free(access);
        if (rc != 0) {
            errno = error;
            return false;
        }
        if (directory) {
            if (metadata->acl_default && *metadata->acl_default) {
                acl_t defaults = bx_metadata_acl_from_text(metadata->acl_default);
                if (!defaults)
                    return false;
                rc = bx_metadata_set_acl(fd, defaults, true, path_fd);
                error = errno;
                acl_free(defaults);
                if (rc != 0) {
                    errno = error;
                    return false;
                }
            }
            else if (!bx_metadata_delete_default_acl(fd))
                return false;
        }
    }
    const struct bx_file_xattr* capabilities = NULL;
    for (size_t i = 0; i < metadata->len; i++) {
        const struct bx_file_xattr* attr = &metadata->xattrs[i];
        if (strcmp(attr->name, "security.capability") == 0) {
            capabilities = attr;
            continue;
        }
        if (!bx_metadata_apply_xattr(attr, fd))
            return false;
    }
    return !capabilities
        || bx_metadata_apply_xattr(capabilities, fd);
}

enum bx_file_restore_result bx_file_restore_fd(const struct bx_file_restore* restore,
                                               int fd,
                                               bool symlink, bool directory) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return BX_FILE_RESTORE_STAT_ERROR;
    bool path_fd = (flags & O_PATH) != 0;
    if (restore->set_owner || restore->set_group) {
        uid_t uid = restore->set_owner ? restore->uid : (uid_t)-1;
        gid_t gid = restore->set_group ? restore->gid : (gid_t)-1;
        int rc = path_fd ? fchownat(fd, "", uid, gid, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)
                         : fchown(fd, uid, gid);
        if (rc != 0)
            return BX_FILE_RESTORE_STAT_ERROR;
    }
    if (restore->set_mode && !symlink) {
        int rc;
        if (path_fd) {
#ifdef SYS_fchmodat2
            rc = (int)syscall(SYS_fchmodat2, fd, "", restore->mode & 07777u, AT_EMPTY_PATH);
#else
            errno = ENOTSUP;
            rc = -1;
#endif
        }
        else
            rc = fchmod(fd, restore->mode & 07777u);
        if (rc != 0)
            return BX_FILE_RESTORE_STAT_ERROR;
    }
    if (!bx_file_metadata_apply(&restore->metadata, fd, symlink, directory,
                                 restore->mode))
        return BX_FILE_RESTORE_METADATA_ERROR;
    if (restore->set_mtime) {
        struct timespec times[2] = {restore->mtime, restore->mtime};
        int rc = path_fd ? utimensat(fd, "", times, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)
                         : futimens(fd, times);
        if (rc != 0)
            return BX_FILE_RESTORE_STAT_ERROR;
    }
    return bx_file_metadata_apply_flags(fd, restore->flags_set, restore->flags_clear) ? BX_FILE_RESTORE_OK : BX_FILE_RESTORE_METADATA_ERROR;
}
