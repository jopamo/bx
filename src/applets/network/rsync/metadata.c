/* SPDX-License-Identifier: ISC */
#include "config.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/openat2.h>
#include <sys/xattr.h>
#include "lib/open_resolve.h"
#endif

#include "extern.h"
#include "metadata.h"

#define META_MAGIC UINT32_C(0x35395842) /* BX95, little endian */
#define META_ACL 1U
#define META_XATTR 2U
#define META_ALL_XATTRS 4U
#define META_COUNT_MAX 1024U
#define META_NAME_MAX 255U
#define META_VALUE_MAX 65536U
#define META_FILE_MAX (1024U * 1024U)
#define META_SESSION_MAX (256U * 1024U * 1024U)

struct meta_attr {
	char *name;
	unsigned char *value;
	size_t size;
};

struct bx_rsync_meta {
	struct bx_rsync_meta *next;
	struct meta_attr *attrs;
	size_t count;
	size_t bytes;
};

static uint32_t
get32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	    (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned int
get16(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8;
}

static void
put32(unsigned char *p, uint32_t v)
{
	for (unsigned int i = 0; i < 4; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

static int
is_acl(const char *name)
{
	return strcmp(name, "system.posix_acl_access") == 0 ||
	    strcmp(name, "system.posix_acl_default") == 0;
}

/* The same selection rule governs capture, decoding, comparison and removal. */
static int
selected(unsigned int flags, const char *name)
{
	if (is_acl(name))
		return (flags & META_ACL) != 0;
	if (!(flags & META_XATTR))
		return 0;
	if (strncmp(name, "user.", 5) == 0 && name[5] != '\0')
		return 1;
	return (flags & META_ALL_XATTRS) &&
	    ((strncmp(name, "security.", 9) == 0 && name[9] != '\0') ||
	     (strncmp(name, "trusted.", 8) == 0 && name[8] != '\0'));
}

/*
 * Linux POSIX ACL xattrs are version 2, with little-endian tag, permissions
 * and ID fields. Require canonical tag/ID order before accepting an ACL.
 */
static int
acl_valid(const struct meta_attr *a, mode_t mode)
{
	unsigned int seen = 0, last = 0;
	uint32_t lastid = 0;
	int named = 0;

	if (!is_acl(a->name))
		return 1;
	if (S_ISLNK(mode) ||
	    (strcmp(a->name, "system.posix_acl_default") == 0 && !S_ISDIR(mode)) ||
	    a->size < 28 || (a->size - 4) % 8 != 0 || get32(a->value) != 2)
		return 0;
	for (size_t i = 4; i < a->size; i += 8) {
		unsigned int tag = get16(a->value + i);
		uint32_t id = get32(a->value + i + 4);

		if (get16(a->value + i + 2) > 7 || tag < last)
			return 0;
		switch (tag) {
		case 2: /* ACL_USER */
		case 8: /* ACL_GROUP */
			if (id == UINT32_MAX || (tag == last && id <= lastid))
				return 0;
			named = 1;
			break;
		case 1: /* ACL_USER_OBJ */
		case 4: /* ACL_GROUP_OBJ */
		case 16: /* ACL_MASK */
		case 32: /* ACL_OTHER */
			if ((seen & tag) || id != UINT32_MAX)
				return 0;
			seen |= tag;
			break;
		default:
			return 0;
		}
		last = tag;
		lastid = id;
	}
	return (seen & 37) == 37 && (!named || (seen & 16));
}

static void
acl_set_mode(struct meta_attr *a, mode_t mode)
{
	int mask = 0;

	if (strcmp(a->name, "system.posix_acl_access") != 0)
		return;
	for (size_t i = 4; i < a->size; i += 8)
		mask |= get16(a->value + i) == 16;
	for (size_t i = 4; i < a->size; i += 8) {
		unsigned int tag = get16(a->value + i), perms;
		if (tag == 1)
			perms = (mode >> 6) & 7;
		else if (tag == 16 || (tag == 4 && !mask))
			perms = (mode >> 3) & 7;
		else if (tag == 32)
			perms = mode & 7;
		else
			continue;
		a->value[i + 2] = (unsigned char)perms;
		a->value[i + 3] = 0;
	}
}

static void
meta_free(struct bx_rsync_meta *m)
{
	if (m == NULL)
		return;
	for (size_t i = 0; i < m->count; i++) {
		free(m->attrs[i].name);
		free(m->attrs[i].value);
	}
	free(m->attrs);
	free(m);
}

void
bx_rsync_meta_cleanup(struct sess *sess)
{
	while (sess->metadata_pool != NULL) {
		struct bx_rsync_meta *m = sess->metadata_pool;
		sess->metadata_pool = m->next;
		meta_free(m);
	}
}

int
bx_rsync_meta_negotiate(struct sess *sess, int fdin, int fdout)
{
	uint32_t magic, flags, role, ours;
	int sender;

	if (sess->opts == NULL || !sess->opts->bx_protocol)
		return 1;
	sender = sess->opts->server ? sess->opts->sender :
	    sess->mode == FARGS_SENDER;
	if (sess->protocol != 29) {
		ERRX("BX/29.5 requires base protocol 29");
		return 0;
	}
#ifndef __linux__
	ERRX("BX/29.5 requires Linux");
	return 0;
#endif
	if (access("/proc/self/fd", X_OK) == -1) {
		ERR("BX/29.5 requires /proc/self/fd");
		return 0;
	}
	ours = (sess->opts->preserve_acls ? META_ACL : 0) |
	    (sess->opts->preserve_xattrs ? META_XATTR : 0);
	if (sender && (ours & META_XATTR) && geteuid() == 0)
		ours |= META_ALL_XATTRS;
	if (!io_write_uint(sess, fdout, META_MAGIC) ||
	    !io_write_uint(sess, fdout, ours) ||
	    !io_write_uint(sess, fdout, sender ? 1U : 2U) ||
	    !io_read_uint(sess, fdin, &magic) ||
	    !io_read_uint(sess, fdin, &flags) ||
	    !io_read_uint(sess, fdin, &role))
		return 0;
	if (magic != META_MAGIC || role != (sender ? 2U : 1U) ||
	    (flags & ~(META_ACL | META_XATTR | META_ALL_XATTRS)) != 0 ||
	    ((flags ^ ours) & (META_ACL | META_XATTR)) != 0 ||
	    ((flags & META_ALL_XATTRS) && (sender || !(flags & META_XATTR)))) {
		ERRX("peer did not agree to BX/29.5 metadata options");
		return 0;
	}
	sess->metadata_flags = sender ? ours : flags;
	return 1;
}

static int
attr_cmp(const void *a, const void *b)
{
	const struct meta_attr *aa = a, *bb = b;
	return strcmp(aa->name, bb->name);
}

static struct meta_attr *
meta_find(const struct bx_rsync_meta *m, const char *name)
{
	for (size_t i = 0; i < m->count; i++)
		if (strcmp(m->attrs[i].name, name) == 0)
			return &m->attrs[i];
	return NULL;
}

static int
meta_add(struct bx_rsync_meta *m, const char *name, size_t size)
{
	size_t len = strlen(name);
	size_t cost = sizeof(struct meta_attr) + len + 1 + (size ? size : 1);
	struct meta_attr *p;

	if (len == 0 || len > META_NAME_MAX || size > META_VALUE_MAX ||
	    m->count == META_COUNT_MAX || cost > META_FILE_MAX - m->bytes) {
		errno = E2BIG;
		return 0;
	}
	p = reallocarray(m->attrs, m->count + 1, sizeof(*p));
	if (p == NULL)
		return 0;
	m->attrs = p;
	p = &m->attrs[m->count++];
	memset(p, 0, sizeof(*p));
	p->name = strdup(name);
	p->value = malloc(size ? size : 1);
	p->size = size;
	m->bytes += cost;
	return p->name != NULL && p->value != NULL;
}

#ifdef __linux__
/*
 * Pin non-symlinks with O_PATH; using the proc descriptor avoids opening
 * devices and FIFOs for I/O. For symlinks, pin the parent and use l*xattr:
 * the last path component must never be followed.
 */
struct meta_file {
	int fd;
	int parent;
	int symlink;
	char path[PATH_MAX];
};

static void
file_close(struct meta_file *f)
{
	int saved = errno;
	if (f->fd != -1)
		close(f->fd);
	if (f->parent != -1)
		close(f->parent);
	errno = saved;
}

static int
file_open(struct meta_file *out, int fd, int root, const char *path,
    mode_t mode, int source)
{
	struct stat st;
	char *parent, *leaf;
	int n;

	memset(out, 0, sizeof(*out));
	out->fd = out->parent = -1;
	if (fd != -1) {
		n = snprintf(out->path, sizeof(out->path), "/proc/self/fd/%d", fd);
		return n > 0 && (size_t)n < sizeof(out->path);
	}
	if (source)
		out->fd = openat(root, path, O_PATH | O_CLOEXEC |
		    (S_ISLNK(mode) ? O_NOFOLLOW : 0));
	else
		out->fd = bx_openat_resolve(root, path,
		    O_PATH | O_CLOEXEC | O_NOFOLLOW, 0,
		    RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS);
	if (out->fd == -1 || fstat(out->fd, &st) == -1)
		goto fail;
	if ((st.st_mode & S_IFMT) != (mode & S_IFMT)) {
		errno = ESTALE;
		goto fail;
	}
	out->symlink = S_ISLNK(st.st_mode);
	if (!out->symlink) {
		n = snprintf(out->path, sizeof(out->path), "/proc/self/fd/%d", out->fd);
		return n > 0 && (size_t)n < sizeof(out->path);
	}
	parent = strdup(path);
	if (parent == NULL)
		goto fail;
	leaf = strrchr(parent, '/');
	if (leaf != NULL) {
		*leaf++ = '\0';
		out->parent = source ?
		    openat(root, *parent ? parent : "/", O_PATH | O_DIRECTORY | O_CLOEXEC) :
		    bx_openat_resolve(root, *parent ? parent : ".",
		    O_PATH | O_DIRECTORY | O_CLOEXEC, 0,
		    RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS);
	} else {
		leaf = parent;
		out->parent = openat(root, ".", O_PATH | O_DIRECTORY | O_CLOEXEC);
	}
	n = snprintf(out->path, sizeof(out->path), "/proc/self/fd/%d/%s",
	    out->parent, leaf);
	free(parent);
	if (out->parent == -1)
		goto fail;
	if (n < 0 || (size_t)n >= sizeof(out->path)) {
		errno = ENAMETOOLONG;
		goto fail;
	}
	return 1;
fail:
	file_close(out);
	return 0;
}

static ssize_t
attr_get(const struct meta_file *f, const char *name, void *buf, size_t size)
{
	return f->symlink ? lgetxattr(f->path, name, buf, size) :
	    getxattr(f->path, name, buf, size);
}

static int
meta_capture(struct bx_rsync_meta *m, const struct meta_file *f,
    unsigned int flags, mode_t mode)
{
	char *names;
	ssize_t n = f->symlink ? llistxattr(f->path, NULL, 0) :
	    listxattr(f->path, NULL, 0);

	if (n == -1 && (errno == ENOTSUP || errno == EOPNOTSUPP))
		return 1;
	if (n == -1)
		return 0;
	if (n > META_VALUE_MAX) {
		errno = E2BIG;
		return 0;
	}
	if (n == 0)
		return 1;
	names = malloc((size_t)n);
	if (names == NULL)
		return 0;
	n = f->symlink ? llistxattr(f->path, names, (size_t)n) :
	    listxattr(f->path, names, (size_t)n);
	if (n == -1)
		goto fail;
	for (size_t i = 0; i < (size_t)n;) {
		size_t len = strnlen(names + i, (size_t)n - i);
		const char *name = names + i;
		ssize_t size;
		struct meta_attr *a;

		if (len == 0 || len == (size_t)n - i) {
			errno = EINVAL;
			goto fail;
		}
		i += len + 1;
		if (!selected(flags, name))
			continue;
		size = attr_get(f, name, NULL, 0);
		if (size == -1 || !meta_add(m, name, (size_t)size))
			goto fail;
		a = &m->attrs[m->count - 1];
		if (attr_get(f, name, a->value, a->size) != size) {
			errno = ESTALE;
			goto fail;
		}
		if (!acl_valid(a, mode)) {
			errno = EINVAL;
			goto fail;
		}
	}
	free(names);
	if (m->count > 1)
		qsort(m->attrs, m->count, sizeof(*m->attrs), attr_cmp);
	return 1;
fail:
	free(names);
	return 0;
}

static int
attr_equal(const struct meta_attr *a, const struct meta_attr *b)
{
	return a != NULL && b != NULL && a->size == b->size &&
	    memcmp(a->value, b->value, a->size) == 0;
}

static int
meta_restore(const struct bx_rsync_meta *wanted, const struct bx_rsync_meta *have,
    const struct meta_file *file, int acls)
{
	for (size_t i = 0; i < have->count; i++) {
		const struct meta_attr *a = &have->attrs[i];
		if (is_acl(a->name) != acls || meta_find(wanted, a->name) != NULL)
			continue;
		int rc = file->symlink ? lremovexattr(file->path, a->name) :
		    removexattr(file->path, a->name);
		if (rc == -1 && errno != ENODATA)
			return 0;
	}
	for (size_t i = 0; i < wanted->count; i++) {
		const struct meta_attr *a = &wanted->attrs[i];
		if (is_acl(a->name) != acls ||
		    attr_equal(a, meta_find(have, a->name)))
			continue;
		int rc = file->symlink ? lsetxattr(file->path, a->name,
		    a->value, a->size, 0) :
		    setxattr(file->path, a->name, a->value, a->size, 0);
		if (rc == -1)
			return 0;
	}
	return 1;
}


#endif

int
bx_rsync_meta_send(struct sess *sess, int fd, const struct flist *f,
    struct ident **uids, size_t *uidsz, struct ident **gids, size_t *gidsz)
{
	struct bx_rsync_meta *m;
	int rc = 0;

	if (!sess->opts->bx_protocol)
		return 1;
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return 0;
#ifdef __linux__
	struct meta_file file;
	struct stat st;
	if (!file_open(&file, -1, AT_FDCWD, f->path, f->st.mode, 1))
		goto out;
	if (fstat(file.fd, &st) != -1) {
		if (st.st_dev != (dev_t)f->st.device ||
		    st.st_ino != (ino_t)f->st.inode)
			errno = ESTALE;
		else
			rc = meta_capture(m, &file, sess->metadata_flags, f->st.mode);
	}
	file_close(&file);
	if (!rc)
		goto out;
	rc = 0;
#else
	errno = ENOTSUP;
	goto out;
#endif
	if (m->bytes + sizeof(*m) > META_SESSION_MAX - sess->metadata_bytes) {
		errno = E2BIG;
		goto out;
	}
	sess->metadata_bytes += m->bytes + sizeof(*m);
	if (!io_write_uint(sess, fd, (uint32_t)m->count))
		goto out;
	for (size_t i = 0; i < m->count; i++) {
		struct meta_attr *a = &m->attrs[i];
		size_t len = strlen(a->name);

		if (is_acl(a->name) && !sess->opts->numeric_ids) {
			for (size_t j = 4; j < a->size; j += 8) {
				unsigned int tag = get16(a->value + j);
				int32_t id = (int32_t)get32(a->value + j + 4);
				if ((tag == 2 && !idents_add(0, uids, uidsz, id)) ||
				    (tag == 8 && !idents_add(1, gids, gidsz, id)))
					goto out;
			}
		}
		if (!io_write_uint(sess, fd, (uint32_t)len) ||
		    !io_write_uint(sess, fd, (uint32_t)a->size) ||
		    !io_write_buf(sess, fd, a->name, len) ||
		    !io_write_buf(sess, fd, a->value, a->size))
			goto out;
	}
	rc = 1;
out:
	if (!rc)
		ERR("%s: cannot send BX/29.5 metadata", f->path);
	meta_free(m);
	return rc;
}

int
bx_rsync_meta_recv(struct sess *sess, int fd, struct flist *f)
{
	struct bx_rsync_meta *m;
	uint32_t count, len, size;
	char name[META_NAME_MAX + 1];

	if (!sess->opts->bx_protocol)
		return 1;
	m = calloc(1, sizeof(*m));
	if (m == NULL)
		return 0;
	if (!io_read_uint(sess, fd, &count) || count > META_COUNT_MAX)
		goto fail;
	for (uint32_t i = 0; i < count; i++) {
		struct meta_attr *a;
		if (!io_read_uint(sess, fd, &len) || len == 0 || len > META_NAME_MAX ||
		    !io_read_uint(sess, fd, &size) || size > META_VALUE_MAX ||
		    !io_read_buf(sess, fd, name, len))
			goto fail;
		name[len] = '\0';
		if (memchr(name, '\0', len) != NULL ||
		    !selected(sess->metadata_flags, name) ||
		    (i != 0 && strcmp(m->attrs[i - 1].name, name) >= 0) ||
		    !meta_add(m, name, size))
			goto fail;
		a = &m->attrs[i];
		if (!io_read_buf(sess, fd, a->value, size) || !acl_valid(a, f->st.mode))
			goto fail;
		if (sess->opts->preserve_perms)
			acl_set_mode(a, f->st.mode);
	}
	if (m->bytes + sizeof(*m) > META_SESSION_MAX - sess->metadata_bytes)
		goto fail;
	sess->metadata_bytes += m->bytes + sizeof(*m);
	m->next = sess->metadata_pool;
	sess->metadata_pool = m;
	f->metadata = m;
	return 1;
fail:
	ERRX("%s: invalid or oversized BX/29.5 metadata", f->path);
	meta_free(m);
	return 0;
}

static int
acl_entry_cmp(const void *a, const void *b)
{
	const unsigned char *aa = a, *bb = b;
	unsigned int ta = get16(aa), tb = get16(bb);
	uint32_t ia = get32(aa + 4), ib = get32(bb + 4);
	if (ta != tb)
		return ta < tb ? -1 : 1;
	return ia < ib ? -1 : ia != ib;
}

int
bx_rsync_meta_remap(struct sess *sess, const struct ident *uids, size_t uidsz,
    const struct ident *gids, size_t gidsz)
{
	if (!sess->opts->preserve_acls || sess->opts->numeric_ids)
		return 1;
	for (struct bx_rsync_meta *m = sess->metadata_pool; m != NULL; m = m->next) {
		for (size_t i = 0; i < m->count; i++) {
			struct meta_attr *a = &m->attrs[i];
			if (!is_acl(a->name))
				continue;
			for (size_t j = 4; j < a->size; j += 8) {
				unsigned int tag = get16(a->value + j);
				uint32_t id = get32(a->value + j + 4);
				const struct ident *ids = tag == 2 ? uids : gids;
				size_t n = tag == 2 ? uidsz : gidsz;
				if ((tag != 2 && tag != 8) || id == 0)
					continue;
				for (size_t k = 0; k < n; k++)
					if ((uint32_t)ids[k].id == id) {
						put32(a->value + j + 4, (uint32_t)ids[k].mapped);
						break;
					}
			}
			qsort(a->value + 4, (a->size - 4) / 8, 8, acl_entry_cmp);
			if (!acl_valid(a, S_IFDIR)) {
				ERRX("ACL identifiers collide after name mapping");
				return 0;
			}
		}
	}
	return 1;
}

int
bx_rsync_meta_apply(struct sess *sess, int fd, int root, const struct flist *f,
    const char *path, int acls)
{
	if (!sess->opts->bx_protocol ||
	    !(sess->metadata_flags & (acls ? META_ACL : META_XATTR)) ||
	    sess->opts->dry_run || (f->flstate & FLIST_SKIP_METADATA))
		return 1;
	if (f->metadata == NULL) {
		errno = EINVAL;
		goto fail;
	}
#ifdef __linux__
	struct meta_file file;
	struct stat st;
	struct bx_rsync_meta *have = calloc(1, sizeof(*have));
	unsigned int flags = acls ? META_ACL : sess->metadata_flags & ~META_ACL;
	int rc = 0, changed_mode = 0, saved;
	if (have == NULL)
		goto fail;
	if (!file_open(&file, fd, root, path, f->st.mode, 0)) {
		meta_free(have);
		goto fail;
	}
	/* user.* access requires read/write permission even for the owner. */
	if (!acls && !file.symlink) {
		if (stat(file.path, &st) == -1)
			goto close_file;
		if (st.st_uid == geteuid() && (st.st_mode & 0600) != 0600) {
			if (chmod(file.path, st.st_mode | 0600) == -1)
				goto close_file;
			changed_mode = 1;
		}
	}
	rc = meta_capture(have, &file, flags, f->st.mode) &&
	    meta_restore(f->metadata, have, &file, acls);
	saved = errno;
	if (changed_mode && chmod(file.path, st.st_mode) == -1)
		rc = 0;
	else
		errno = saved;
close_file:
	file_close(&file);
	meta_free(have);
	if (rc)
		return 1;
#else
	(void)fd;
	(void)root;
	errno = ENOTSUP;
#endif
fail:
	ERR("%s: cannot restore %s", path, acls ? "ACLs" : "extended attributes");
	sess->total_errors++;
	return 0;
}

int
bx_rsync_meta_compare(struct sess *sess, int root, const struct flist *f,
    const char *path, int32_t *flags)
{
	if (!sess->opts->bx_protocol || f->metadata == NULL)
		return 1;
#ifdef __linux__
	struct meta_file file;
	struct bx_rsync_meta *have = calloc(1, sizeof(*have));
	int rc;
	if (have == NULL)
		goto fail;
	if (!file_open(&file, -1, root, path, f->st.mode, 0)) {
		meta_free(have);
		goto fail;
	}
	rc = meta_capture(have, &file, sess->metadata_flags, f->st.mode);
	file_close(&file);
	if (rc) {
		const struct bx_rsync_meta *sets[] = { have, f->metadata };
		for (size_t s = 0; s < 2; s++)
			for (size_t i = 0; i < sets[s]->count; i++) {
				const struct meta_attr *a = &sets[s]->attrs[i];
				if (!attr_equal(a, meta_find(sets[1 - s], a->name)))
					*flags |= is_acl(a->name) ? IFLAG_ACL : IFLAG_XATTR;
			}
	}
	meta_free(have);
	if (rc)
		return 1;
#else
	(void)root;
	(void)flags;
	errno = ENOTSUP;
#endif
fail:
	ERR("%s: cannot compare BX/29.5 metadata", path);
	sess->total_errors++;
	return 0;
}
