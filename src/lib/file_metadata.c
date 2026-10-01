#define _GNU_SOURCE
#include "lib/file_metadata.h"

#include <acl/libacl.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/capability.h>
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

enum bx_file_xattr_class bx_file_xattr_classify(const char* name) {
    if (strcmp(name, "system.posix_acl_access") == 0)
        return BX_FILE_XATTR_ACL_ACCESS;
    if (strcmp(name, "system.posix_acl_default") == 0)
        return BX_FILE_XATTR_ACL_DEFAULT;
    if (strcmp(name, "security.selinux") == 0)
        return BX_FILE_XATTR_SELINUX;
    if (strcmp(name, "security.capability") == 0)
        return BX_FILE_XATTR_CAPABILITY;
    return BX_FILE_XATTR_ORDINARY;
}

struct bx_metadata_capabilities {
    uint64_t permitted;
    uint64_t inheritable;
    uint32_t root;
    bool effective;
};

static uint32_t bx_metadata_le32(const unsigned char* data) {
    uint32_t value;
    memcpy(&value, data, sizeof(value));
    return le32toh(value);
}

static bool bx_metadata_cap_decode(const void* value, size_t size, struct bx_metadata_capabilities* caps) {
    if (!value || size < 4) {
        errno = EINVAL;
        return false;
    }
    const unsigned char* data = value;
    uint32_t magic = bx_metadata_le32(data), revision = magic & VFS_CAP_REVISION_MASK;
    size_t required;
    switch (revision) {
        case VFS_CAP_REVISION_1:
            required = XATTR_CAPS_SZ_1;
            break;
        case VFS_CAP_REVISION_2:
            required = XATTR_CAPS_SZ_2;
            break;
        case VFS_CAP_REVISION_3:
            required = XATTR_CAPS_SZ_3;
            break;
        default:
            errno = EOPNOTSUPP;
            return false;
    }
    if (size != required) {
        errno = EINVAL;
        return false;
    }
    if (magic & ~(VFS_CAP_REVISION_MASK | VFS_CAP_FLAGS_EFFECTIVE)) {
        errno = EOPNOTSUPP;
        return false;
    }
    *caps = (struct bx_metadata_capabilities){
        .permitted = bx_metadata_le32(data + 4),
        .inheritable = bx_metadata_le32(data + 8),
        .effective = (magic & VFS_CAP_FLAGS_EFFECTIVE) != 0,
    };
    if (revision != VFS_CAP_REVISION_1) {
        caps->permitted |= (uint64_t)bx_metadata_le32(data + 12) << 32;
        caps->inheritable |= (uint64_t)bx_metadata_le32(data + 16) << 32;
    }
    if (revision == VFS_CAP_REVISION_3) {
        caps->root = bx_metadata_le32(data + 20);
        if (caps->root == UINT32_MAX) {
            errno = EINVAL;
            return false;
        }
    }
    return true;
}

static size_t bx_metadata_label_length(const unsigned char* value, size_t size) {
    return size && value[size - 1] == 0 ? size - 1 : size;
}

bool bx_file_xattr_validate(enum bx_file_xattr_class kind, const void* value, size_t size) {
    if (kind == BX_FILE_XATTR_ORDINARY)
        return true;
    if (kind == BX_FILE_XATTR_CAPABILITY) {
        struct bx_metadata_capabilities caps;
        return bx_metadata_cap_decode(value, size, &caps);
    }
    if (kind == BX_FILE_XATTR_SELINUX && value && size && size <= 65536u) {
        size_t length = bx_metadata_label_length(value, size);
        if (length && !memchr(value, 0, length))
            return true;
    }
    errno = EINVAL;
    return false;
}

bool bx_file_metadata_stat_unchanged(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_mode == b->st_mode && a->st_uid == b->st_uid && a->st_gid == b->st_gid && a->st_nlink == b->st_nlink && a->st_rdev == b->st_rdev &&
           a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

bool bx_file_metadata_target_fd(struct bx_file_metadata_target* target, int fd) {
    struct bx_file_metadata_target value = {.fd = fd};
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fstat(fd, &value.status) != 0)
        return false;
    if ((flags & O_PATH) || (!S_ISREG(value.status.st_mode) && !S_ISDIR(value.status.st_mode))) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (!bx_mount_identity_read(fd, &value.identity))
        return false;
    *target = value;
    return true;
}

bool bx_file_metadata_target_leaf(struct bx_file_metadata_target* target, int parent_fd, const char* name, const struct stat* expected) {
    if (parent_fd < 0 || !name || !*name || strchr(name, '/') || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || !expected) {
        errno = EINVAL;
        return false;
    }
    struct stat parent;
    if (fstat(parent_fd, &parent) != 0)
        return false;
    if (!S_ISDIR(parent.st_mode)) {
        errno = ENOTDIR;
        return false;
    }
    struct bx_file_metadata_target value = {.fd = parent_fd, .name = name};
    if (fstatat(parent_fd, name, &value.status, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    if (!bx_file_metadata_stat_unchanged(expected, &value.status)) {
        errno = ESTALE;
        return false;
    }
    if (!S_ISLNK(value.status.st_mode) && !S_ISFIFO(value.status.st_mode) && !S_ISCHR(value.status.st_mode) && !S_ISBLK(value.status.st_mode)) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (!bx_mount_identity_read_at(parent_fd, name, &value.identity))
        return false;
    if (!value.identity.has_mount) {
        errno = EOPNOTSUPP;
        return false;
    }
    *target = value;
    return bx_file_metadata_target_verify(target, true);
}

bool bx_file_metadata_target_verify(const struct bx_file_metadata_target* target, bool unchanged) {
    struct stat status;
    struct bx_mount_identity identity;
    int rc = target->name ? fstatat(target->fd, target->name, &status, AT_SYMLINK_NOFOLLOW) : fstat(target->fd, &status);
    if (rc != 0) {
        if (target->name && errno == ENOENT)
            errno = ESTALE;
        return false;
    }
    if (!(target->name ? bx_mount_identity_read_at(target->fd, target->name, &identity) : bx_mount_identity_read(target->fd, &identity)))
        return false;
    bool same;
    if (!bx_mount_identity_compare(&target->identity, &identity, &same))
        return false;
    if (!same || status.st_ino != target->status.st_ino || (status.st_mode & S_IFMT) != (target->status.st_mode & S_IFMT) ||
        (unchanged && !bx_file_metadata_stat_unchanged(&target->status, &status))) {
        errno = ESTALE;
        return false;
    }
    return true;
}

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

static ssize_t bx_metadata_get_xattr(const struct bx_file_metadata_target* target, const char* name, void* value, size_t size);

static acl_t bx_metadata_acl_decode(const unsigned char* data, size_t size, acl_type_t type) {
    acl_t acl = NULL;
    uint32_t version;
    if (size < 4 || size > 65536u || (size - 4) % 8 != 0 || (type == ACL_TYPE_ACCESS && size == 4))
        goto invalid;
    memcpy(&version, data, sizeof(version));
    if (le32toh(version) != 2)
        goto invalid;
    acl = acl_init((int)((size - 4) / 8));
    if (!acl)
        return NULL;
    for (size_t pos = 4; pos < size; pos += 8) {
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
        if (acl_create_entry(&acl, &entry) != 0 || acl_set_tag_type(entry, tag) != 0 || acl_get_permset(entry, &set) != 0 || acl_clear_perms(set) != 0 ||
            ((perms & ACL_READ) && acl_add_perm(set, ACL_READ) != 0) || ((perms & ACL_WRITE) && acl_add_perm(set, ACL_WRITE) != 0) || ((perms & ACL_EXECUTE) && acl_add_perm(set, ACL_EXECUTE) != 0))
            goto fail;
        if (tag == ACL_USER) {
            uid_t uid = (uid_t)id;
            if (id == UINT32_MAX || (uint32_t)uid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &uid) != 0)
                goto fail;
        }
        else if (tag == ACL_GROUP) {
            gid_t gid = (gid_t)id;
            if (id == UINT32_MAX || (uint32_t)gid != id)
                goto invalid;
            if (acl_set_qualifier(entry, &gid) != 0)
                goto fail;
        }
    }
    if (size > 4 && acl_valid(acl) != 0)
        goto fail;
    return acl;
invalid:
    errno = EINVAL;
fail: {
    int error = errno;
    if (acl)
        acl_free(acl);
    errno = error;
    return NULL;
}
}

static char* bx_metadata_acl_text(acl_t acl) {
    char* text = acl_to_any_text(acl, NULL, ',', TEXT_NUMERIC_IDS);
    if (text && strlen(text) > 65536u) {
        acl_free(text);
        errno = E2BIG;
        return NULL;
    }
    return text;
}

/* ENODATA denotes mode-derived access permissions or an empty default ACL. */
static acl_t bx_metadata_acl_target(const struct bx_file_metadata_target* target, acl_type_t type) {
    unsigned char* data = xmalloc(65536u);
    ssize_t size = bx_metadata_get_xattr(target, type == ACL_TYPE_DEFAULT ? "system.posix_acl_default" : "system.posix_acl_access", data, 65536u);
    acl_t acl = size >= 0 ? bx_metadata_acl_decode(data, (size_t)size, type) : NULL;
    if (size < 0 && errno == ENODATA)
        acl = type == ACL_TYPE_DEFAULT ? acl_init(0) : acl_from_mode(target->status.st_mode);
    int error = errno;
    free(data);
    errno = error;
    return acl;
}

bool bx_file_metadata_set_acl_xattr(struct bx_file_metadata* metadata, const char* name, const void* value, size_t size) {
    if (!name || !value) {
        errno = EINVAL;
        return false;
    }
    enum bx_file_xattr_class kind = bx_file_xattr_classify(name);
    if (kind != BX_FILE_XATTR_ACL_ACCESS && kind != BX_FILE_XATTR_ACL_DEFAULT) {
        errno = EINVAL;
        return false;
    }
    acl_t acl = bx_metadata_acl_decode(value, size, kind == BX_FILE_XATTR_ACL_ACCESS ? ACL_TYPE_ACCESS : ACL_TYPE_DEFAULT);
    if (!acl)
        return false;
    char* text = bx_metadata_acl_text(acl);
    int error = errno;
    acl_free(acl);
    if (!text) {
        errno = error;
        return false;
    }
    char** slot = kind == BX_FILE_XATTR_ACL_ACCESS ? &metadata->acl_access : &metadata->acl_default;
    free(*slot);
    *slot = xstrdup(text);
    acl_free(text);
    return true;
}

static bool bx_metadata_read_acl(char** text, const struct bx_file_metadata_target* target, acl_type_t type) {
    acl_t acl = bx_metadata_acl_target(target, type);
    if (!acl)
        return false;
    char* value = bx_metadata_acl_text(acl);
    int error = errno;
    acl_free(acl);
    if (!value) {
        errno = error;
        return false;
    }
    *text = xstrdup(value);
    acl_free(value);
    return true;
}

static ssize_t bx_metadata_list_xattrs(const struct bx_file_metadata_target* target, char* names, size_t size) {
    if (!target->name)
        return flistxattr(target->fd, names, size);
#ifdef SYS_listxattrat
    return syscall(SYS_listxattrat, target->fd, target->name, AT_SYMLINK_NOFOLLOW, names, size);
#else
    errno = EOPNOTSUPP;
    return -1;
#endif
}

static ssize_t bx_metadata_get_xattr(const struct bx_file_metadata_target* target, const char* name, void* value, size_t size) {
    if (!target->name)
        return fgetxattr(target->fd, name, value, size);
#ifdef SYS_getxattrat
    /* Linux xattr_args ABI, including eight-byte alignment on 32-bit targets. */
    struct {
        _Alignas(8) uint64_t value;
        uint32_t size;
        uint32_t flags;
    } args = {.value = (uintptr_t)value, .size = (uint32_t)size};
    return syscall(SYS_getxattrat, target->fd, target->name, AT_SYMLINK_NOFOLLOW, name, &args, sizeof(args));
#else
    errno = EOPNOTSUPP;
    return -1;
#endif
}

bool bx_file_metadata_read_target(struct bx_file_metadata* metadata, const struct bx_file_metadata_target* target, bool acls, bx_file_xattr_filter filter, const void* user) {
    if (!bx_file_metadata_target_verify(target, true))
        return false;
    bool symlink = S_ISLNK(target->status.st_mode);
    bool directory = S_ISDIR(target->status.st_mode);
    if (acls && !symlink) {
        if (!bx_metadata_read_acl(&metadata->acl_access, target, ACL_TYPE_ACCESS) || (directory && !bx_metadata_read_acl(&metadata->acl_default, target, ACL_TYPE_DEFAULT)))
            return false;
    }
    if (!filter)
        return bx_file_metadata_target_verify(target, true);
    /* Linux bounds the name list and each value at 64 KiB. Read each in one
     * syscall, avoiding a size-query/read race when another writer changes it. */
    char* names = xmalloc(65536u);
    unsigned char* value = xmalloc(65536u);
    ssize_t size = bx_metadata_list_xattrs(target, names, 65536u);
    bool ok = size >= 0;
    for (ssize_t pos = 0; ok && pos < size;) {
        const char* name = names + pos;
        size_t length = strnlen(name, (size_t)(size - pos));
        if (length == (size_t)(size - pos) || !length) {
            errno = EINVAL;
            ok = false;
            break;
        }
        pos += (ssize_t)length + 1;
        if (!filter(name, user))
            continue;
        ssize_t len = bx_metadata_get_xattr(target, name, value, 65536u);
        if (len < 0) {
            ok = false;
            break;
        }
        enum bx_file_xattr_class kind = bx_file_xattr_classify(name);
        if (kind == BX_FILE_XATTR_CAPABILITY && !S_ISREG(target->status.st_mode)) {
            errno = EOPNOTSUPP;
            ok = false;
            break;
        }
        ok = kind == BX_FILE_XATTR_ACL_ACCESS || kind == BX_FILE_XATTR_ACL_DEFAULT ? bx_file_metadata_set_acl_xattr(metadata, name, value, (size_t)len)
                                                                                   : bx_file_xattr_validate(kind, value, (size_t)len) && bx_file_metadata_set(metadata, name, value, (size_t)len);
    }
    int error = errno;
    free(value);
    free(names);
    errno = error;
    return ok && bx_file_metadata_target_verify(target, true);
}

/* Star's fourth field is authoritative. Never resolve archive qualifiers
 * against the destination's user/group names. libacl validates numeric range. */
static acl_t bx_metadata_acl_from_text(const char* text) {
    char* copy = xmalloc(strlen(text) + 1u);
    char* out = copy;
    for (const char* in = text; *in;) {
        const char* end = in + strcspn(in, ",\n");
        const char* tag = in + strspn(in, " \t");
        const char* first = memchr(in, ':', (size_t)(end - in));
        const char* second = first ? memchr(first + 1, ':', (size_t)(end - first - 1)) : NULL;
        const char* third = second ? memchr(second + 1, ':', (size_t)(end - second - 1)) : NULL;
        if (second && second > first + 1) {
            bool named = (first - tag == 1 && (*tag == 'u' || *tag == 'g')) || (first - tag == 4 && memcmp(tag, "user", 4) == 0) || (first - tag == 5 && memcmp(tag, "group", 5) == 0);
            const char* id = third ? third + 1 : first + 1;
            size_t length = (size_t)((third ? end : second) - id);
            if (!named || !length || strspn(id, "0123456789") != length)
                goto invalid;
            size_t prefix = (size_t)(first + 1 - in);
            memcpy(out, in, prefix);
            out += prefix;
            memcpy(out, id, length);
            out += length;
            size_t permissions = (size_t)((third ? third : end) - second);
            memcpy(out, second, permissions);
            out += permissions;
        }
        else {
            if (third)
                goto invalid;
            size_t length = (size_t)(end - in);
            memcpy(out, in, length);
            out += length;
        }
        in = end;
        if (*in)
            *out++ = *in++;
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
invalid:
    free(copy);
    errno = EINVAL;
    return NULL;
}

static int bx_metadata_set_xattr(const struct bx_file_metadata_target* target, const char* name, const void* value, size_t size) {
    int rc;
    if (!target->name)
        rc = fsetxattr(target->fd, name, value, size, 0);
    else {
        errno = EOPNOTSUPP;
        rc = -1;
    }
#ifdef __NR_setxattrat
    if (target->name) {
        struct {
            _Alignas(8) uint64_t value;
            uint32_t size;
            uint32_t flags;
        } args = {.value = (uintptr_t)value, .size = (uint32_t)size};
        rc = (int)syscall(__NR_setxattrat, target->fd, target->name, AT_SYMLINK_NOFOLLOW, name, &args, sizeof(args));
    }
#endif
    return rc;
}

/* libacl has no default-ACL fd setter. Encode the Linux xattr format used
 * by the reader and apply it through the same verified target. */
static int bx_metadata_set_acl(const struct bx_file_metadata_target* target, acl_t acl, bool defaults) {
    if (!defaults && !target->name)
        return acl_set_fd(target->fd, acl);
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
        if (acl_get_entry(acl, i ? ACL_NEXT_ENTRY : ACL_FIRST_ENTRY, &entry) != 1 || acl_get_tag_type(entry, &tag) != 0 || acl_get_permset(entry, &perms) != 0)
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
    rc = bx_metadata_set_xattr(target, defaults ? "system.posix_acl_default" : "system.posix_acl_access", data, size);
out: {
    int error = errno;
    free(data);
    errno = error;
}
    return rc;
}

static bool bx_metadata_delete_default_acl(const struct bx_file_metadata_target* target) {
    int rc;
    if (!target->name)
        rc = fremovexattr(target->fd, "system.posix_acl_default");
    else {
        errno = EOPNOTSUPP;
        rc = -1;
    }
#ifdef __NR_removexattrat
    if (target->name)
        rc = (int)syscall(__NR_removexattrat, target->fd, target->name, AT_SYMLINK_NOFOLLOW, "system.posix_acl_default");
#endif
    return rc == 0 || errno == ENODATA;
}

static bool bx_metadata_apply_xattr(const struct bx_file_xattr* attr, const struct bx_file_metadata_target* target) {
    int rc = bx_metadata_set_xattr(target, attr->name, attr->value, attr->size);
    return rc == 0;
}

static bool bx_metadata_apply_semantic(const struct bx_file_xattr* attr, const struct bx_file_metadata_target* target) {
    if (!bx_metadata_apply_xattr(attr, target))
        return false;
    enum bx_file_xattr_class kind = bx_file_xattr_classify(attr->name);
    size_t capacity = kind == BX_FILE_XATTR_CAPABILITY ? XATTR_CAPS_SZ_3 : 65536u;
    unsigned char* observed = xmalloc(capacity);
    ssize_t size = bx_metadata_get_xattr(target, attr->name, observed, capacity);
    bool ok = size >= 0 && bx_file_xattr_validate(kind, observed, (size_t)size);
    if (ok && kind == BX_FILE_XATTR_CAPABILITY) {
        struct bx_metadata_capabilities expected, actual;
        ok = bx_metadata_cap_decode(attr->value, attr->size, &expected) && bx_metadata_cap_decode(observed, (size_t)size, &actual);
        if (ok)
            ok = expected.permitted == actual.permitted && expected.inheritable == actual.inheritable && expected.root == actual.root && expected.effective == actual.effective;
    }
    else if (ok) {
        size_t expected = bx_metadata_label_length(attr->value, attr->size), actual = bx_metadata_label_length(observed, (size_t)size);
        ok = expected == actual && memcmp(attr->value, observed, expected) == 0;
    }
    if (size >= 0 && !ok && bx_file_xattr_validate(kind, observed, (size_t)size))
        errno = EOPNOTSUPP;
    int error = errno;
    free(observed);
    errno = error;
    return ok;
}

static bool bx_metadata_apply(const struct bx_file_metadata* metadata, const struct bx_file_metadata_target* target, bool symlink, bool directory, mode_t mode) {
    int rc, error;
    if ((metadata->restore_acls & BX_FILE_ACL_ACCESS) && !symlink) {
        acl_t access = metadata->acl_access ? bx_metadata_acl_from_text(metadata->acl_access) : acl_from_mode(mode & 0777u);
        if (!access)
            return false;
        rc = bx_metadata_set_acl(target, access, false);
        error = errno;
        acl_free(access);
        if (rc != 0) {
            errno = error;
            return false;
        }
    }
    if ((metadata->restore_acls & BX_FILE_ACL_DEFAULT) && directory) {
        if (metadata->acl_default && *metadata->acl_default) {
            acl_t defaults = bx_metadata_acl_from_text(metadata->acl_default);
            if (!defaults)
                return false;
            rc = bx_metadata_set_acl(target, defaults, true);
            error = errno;
            acl_free(defaults);
            if (rc != 0) {
                errno = error;
                return false;
            }
        }
        else if (!bx_metadata_delete_default_acl(target))
            return false;
    }
    const struct bx_file_xattr* capabilities = NULL;
    const struct bx_file_xattr* selinux = NULL;
    for (size_t i = 0; i < metadata->len; i++) {
        const struct bx_file_xattr* attr = &metadata->xattrs[i];
        enum bx_file_xattr_class kind = bx_file_xattr_classify(attr->name);
        if (kind == BX_FILE_XATTR_CAPABILITY) {
            capabilities = attr;
            continue;
        }
        if (kind == BX_FILE_XATTR_SELINUX) {
            selinux = attr;
            continue;
        }
        if (!bx_metadata_apply_xattr(attr, target))
            return false;
    }
    return (!selinux || bx_metadata_apply_semantic(selinux, target)) && (!capabilities || bx_metadata_apply_semantic(capabilities, target));
}

static bool bx_metadata_supported(const struct bx_file_metadata* metadata, mode_t type) {
    if (metadata->restore_acls & ~(unsigned int)BX_FILE_ACL_ALL) {
        errno = EINVAL;
        return false;
    }
    if (((metadata->restore_acls & BX_FILE_ACL_ACCESS) && metadata->acl_access && type == S_IFLNK) || ((metadata->restore_acls & BX_FILE_ACL_DEFAULT) && metadata->acl_default && type != S_IFDIR)) {
        errno = EOPNOTSUPP;
        return false;
    }
    for (size_t i = 0; i < metadata->len; i++) {
        enum bx_file_xattr_class kind = bx_file_xattr_classify(metadata->xattrs[i].name);
        if (kind == BX_FILE_XATTR_ACL_ACCESS || kind == BX_FILE_XATTR_ACL_DEFAULT) {
            errno = EINVAL;
            return false;
        }
        if (kind == BX_FILE_XATTR_CAPABILITY && type != S_IFREG) {
            errno = EOPNOTSUPP;
            return false;
        }
        if (!bx_file_xattr_validate(kind, metadata->xattrs[i].value, metadata->xattrs[i].size))
            return false;
    }
    return true;
}

bool bx_file_restore_leaf_supported(const struct bx_file_restore* restore, mode_t type) {
    if (type != S_IFLNK && type != S_IFIFO && type != S_IFCHR && type != S_IFBLK) {
        errno = EINVAL;
        return false;
    }
    if (!bx_metadata_supported(&restore->metadata, type))
        return false;
    if (restore->flags_set || restore->flags_clear) {
        errno = EOPNOTSUPP;
        return false;
    }
    if (restore->set_mode && type != S_IFLNK) {
#ifdef SYS_fchmodat2
        int rc = (int)syscall(SYS_fchmodat2, -1, "leaf", restore->mode & 07777u, AT_SYMLINK_NOFOLLOW);
        if (rc != -1 || errno != EBADF)
            return false;
#else
        errno = EOPNOTSUPP;
        return false;
#endif
    }
    if (restore->metadata.len || (restore->metadata.restore_acls && type != S_IFLNK)) {
#ifdef SYS_setxattrat
        struct {
            _Alignas(8) uint64_t value;
            uint32_t size;
            uint32_t flags;
        } args = {0};
        int rc = (int)syscall(SYS_setxattrat, -1, "leaf", AT_SYMLINK_NOFOLLOW, "user.bx", &args, sizeof(args));
        if (rc != -1 || errno != EBADF)
            return false;
#else
        errno = EOPNOTSUPP;
        return false;
#endif
    }
    for (size_t i = 0; i < restore->metadata.len; i++) {
        if (bx_file_xattr_classify(restore->metadata.xattrs[i].name) != BX_FILE_XATTR_SELINUX)
            continue;
#ifdef SYS_getxattrat
        struct {
            _Alignas(8) uint64_t value;
            uint32_t size;
            uint32_t flags;
        } args = {0};
        int rc = (int)syscall(SYS_getxattrat, -1, "leaf", AT_SYMLINK_NOFOLLOW, "security.selinux", &args, sizeof(args));
        if (rc != -1 || errno != EBADF)
            return false;
#else
        errno = EOPNOTSUPP;
        return false;
#endif
    }
    return true;
}

static enum bx_file_restore_result bx_metadata_restore(const struct bx_file_restore* restore, const struct bx_file_metadata_target* target, bool symlink, bool directory) {
    int fd = target->fd;
    if (restore->set_owner || restore->set_group) {
        uid_t uid = restore->set_owner ? restore->uid : (uid_t)-1;
        gid_t gid = restore->set_group ? restore->gid : (gid_t)-1;
        int rc = target->name ? fchownat(fd, target->name, uid, gid, AT_SYMLINK_NOFOLLOW) : fchown(fd, uid, gid);
        if (rc != 0)
            return BX_FILE_RESTORE_STAT_ERROR;
    }
    if (restore->set_mode && !symlink) {
        if (target->name && !bx_file_metadata_target_verify(target, false))
            return BX_FILE_RESTORE_STAT_ERROR;
        int rc;
        if (target->name) {
#ifdef SYS_fchmodat2
            rc = (int)syscall(SYS_fchmodat2, fd, target->name, restore->mode & 07777u, AT_SYMLINK_NOFOLLOW);
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
    if (target->name && !bx_file_metadata_target_verify(target, false))
        return BX_FILE_RESTORE_STAT_ERROR;
    if (!bx_metadata_apply(&restore->metadata, target, symlink, directory, restore->mode))
        return BX_FILE_RESTORE_METADATA_ERROR;
    if (target->name && !bx_file_metadata_target_verify(target, false))
        return BX_FILE_RESTORE_STAT_ERROR;
    if (restore->set_mtime) {
        struct timespec times[2] = {restore->mtime, restore->mtime};
        int rc = target->name ? utimensat(fd, target->name, times, AT_SYMLINK_NOFOLLOW) : futimens(fd, times);
        if (rc != 0)
            return BX_FILE_RESTORE_STAT_ERROR;
    }
    return bx_file_metadata_apply_flags(fd, restore->flags_set, restore->flags_clear) ? BX_FILE_RESTORE_OK : BX_FILE_RESTORE_METADATA_ERROR;
}

enum bx_file_restore_result bx_file_restore_target(const struct bx_file_restore* restore, const struct bx_file_metadata_target* target) {
    if (!bx_file_metadata_target_verify(target, false))
        return BX_FILE_RESTORE_STAT_ERROR;
    if (!bx_metadata_supported(&restore->metadata, target->status.st_mode & S_IFMT))
        return BX_FILE_RESTORE_METADATA_ERROR;
    if (target->name && !bx_file_restore_leaf_supported(restore, target->status.st_mode & S_IFMT))
        return BX_FILE_RESTORE_METADATA_ERROR;
    enum bx_file_restore_result result = bx_metadata_restore(restore, target, S_ISLNK(target->status.st_mode), S_ISDIR(target->status.st_mode));
    if (result == BX_FILE_RESTORE_OK && !bx_file_metadata_target_verify(target, false))
        return BX_FILE_RESTORE_STAT_ERROR;
    return result;
}

enum bx_file_restore_result bx_file_restore_fd(const struct bx_file_restore* restore, int fd) {
    struct bx_file_metadata_target target;
    if (!bx_file_metadata_target_fd(&target, fd))
        return BX_FILE_RESTORE_STAT_ERROR;
    return bx_file_restore_target(restore, &target);
}

enum bx_file_restore_result bx_file_restore_prepare_regular(const struct bx_file_restore* restore, int fd) {
    struct bx_file_metadata_target target;
    if (!bx_file_metadata_target_fd(&target, fd))
        return BX_FILE_RESTORE_STAT_ERROR;
    if (!S_ISREG(target.status.st_mode)) {
        errno = EINVAL;
        return BX_FILE_RESTORE_STAT_ERROR;
    }
    if (!bx_metadata_supported(&restore->metadata, S_IFREG))
        return BX_FILE_RESTORE_METADATA_ERROR;
    struct bx_file_restore immediate = *restore;
    immediate.flags_set = immediate.flags_clear = 0u;
    immediate.mode = 0600u;
    immediate.set_mode = true;
    immediate.metadata = (struct bx_file_metadata){0};
    enum bx_file_restore_result result = BX_FILE_RESTORE_METADATA_ERROR;
    for (size_t i = 0; i < restore->metadata.len; i++) {
        const struct bx_file_xattr* attr = &restore->metadata.xattrs[i];
        if (bx_file_xattr_classify(attr->name) == BX_FILE_XATTR_ORDINARY && !bx_file_metadata_set(&immediate.metadata, attr->name, attr->value, attr->size))
            goto out;
    }
    result = bx_file_restore_target(&immediate, &target);
    /* Check requested ownership while the inode is private, then return it
     * to the extractor so finalization can reopen a live alias. */
    if (result == BX_FILE_RESTORE_OK && ((restore->set_owner && restore->uid != target.status.st_uid) || (restore->set_group && restore->gid != target.status.st_gid)) &&
        fchown(fd, target.status.st_uid, target.status.st_gid) != 0)
        result = BX_FILE_RESTORE_STAT_ERROR;
out: {
    int error = errno;
    bx_file_metadata_free(&immediate.metadata);
    errno = error;
    return result;
}
}
