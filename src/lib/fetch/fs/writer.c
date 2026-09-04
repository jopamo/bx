#define _GNU_SOURCE
#include "lib/fetch/secure_path.h"
#include "lib/fetch/writer.h"
#include "lib/fetch/xattr.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

struct BxFetchWriter {
    char* path;
    char* parent_path;
    char* basename;
    char* temp_name;
    char* superseded_sidecar_name;
    int fd;
    int parent_fd;
    dev_t parent_dev;
    ino_t parent_ino;
    size_t written;
    int backups;
    bool unlink_existing;
    bool rotate_backups_on_commit;
    bool honor_unlink_on_commit;
    bool exclusive_final_path;
    bool to_stdout;
    bool initial_dest_existed;
    dev_t initial_dest_dev;
    ino_t initial_dest_ino;
    mode_t initial_dest_mode;
    time_t initial_dest_mtime;
    BxFetchMetadata pending_metadata;
    bool has_pending_metadata;
    bool has_pending_mtime;
    time_t pending_mtime;
};

static bool same_destination_identity(const BxFetchWriter* w, const struct stat* st);

static mode_t process_umask(void) {
    mode_t mask = umask(0);
    umask(mask);
    return mask;
}

int bx_fetch_writer_check_secure_path_resolution(void) {
    return bx_fetch_secure_path_check_resolution();
}

int bx_fetch_writer_open_existing_file(const char* path) {
    return bx_fetch_secure_path_open_existing_file(path);
}

static char* parent_path_for_output_path(const char* path) {
    char* parent = NULL;
    char* basename = NULL;
    if (bx_fetch_secure_path_split(path, &parent, &basename) != 0) {
        return NULL;
    }

    free(basename);
    if (parent)
        return parent;
    return strdup(".");
}

static int writer_fail_errno(int error_number) {
    errno = error_number;
    return -1;
}

static int writer_fail_after_close(int error_number, int fd) {
    if (fd != -1)
        close(fd);
    return writer_fail_errno(error_number);
}

static int writer_fail_temp_entry(int error_number, int parent_fd, int fd, char** name_inout) {
    if (fd != -1)
        close(fd);
    if (parent_fd != -1 && name_inout && *name_inout) {
        unlinkat(parent_fd, *name_inout, 0);
        free(*name_inout);
        *name_inout = NULL;
    }
    return writer_fail_errno(error_number);
}

static int capture_parent_directory_identity(BxFetchWriter* w) {
    if (!w || w->parent_fd == -1) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (fstat(w->parent_fd, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    w->parent_dev = st.st_dev;
    w->parent_ino = st.st_ino;
    return 0;
}

static int validate_parent_directory_identity(const BxFetchWriter* w) {
    if (!w || w->to_stdout || !w->path || !w->basename) {
        errno = EINVAL;
        return -1;
    }

    int current_parent_fd = -1;
    char* current_basename = NULL;
    /*
     * The retained parent descriptor is the authority for all mutations.
     * Re-resolve the requested namespace immediately before publication so a
     * renamed parent replaced by a symlink or different directory fails
     * closed; cleanup still targets only the retained directory object.
     */
    current_parent_fd = bx_fetch_secure_path_open_parent_directory(w->path, false, &current_basename);
    if (current_parent_fd == -1) {
        return -1;
    }

    struct stat st;
    int rc = fstat(current_parent_fd, &st);
    int error_number = errno;
    if (rc == 0 && (st.st_dev != w->parent_dev || st.st_ino != w->parent_ino || strcmp(current_basename, w->basename) != 0)) {
        rc = -1;
        error_number = EBUSY;
    }
    if (close(current_parent_fd) != 0 && rc == 0) {
        rc = -1;
        error_number = errno;
    }
    free(current_basename);

    if (rc != 0)
        errno = error_number;
    return rc;
}

int bx_fetch_writer_unlink_file(const char* path) {
    return bx_fetch_secure_path_unlink_file(path);
}

static int open_unique_temp_file_at(int parent_fd, const char* basename, char** temp_name_out) {
    if (parent_fd == -1 || !basename || !temp_name_out) {
        errno = EINVAL;
        return -1;
    }

    static unsigned long long temp_counter = 0;
    mode_t mode = (mode_t)(0644 & ~process_umask());

    for (unsigned long long attempt = 0; attempt < 128; attempt++) {
        char* temp_name = NULL;
        unsigned long long serial = (((unsigned long long)getpid()) << 32) | (temp_counter++);
        if (asprintf(&temp_name, "%s.mira.tmp.%016llx%02llx", basename, serial, attempt) == -1) {
            temp_name = NULL;
        }
        if (!temp_name)
            return -1;

        int fd = bx_fetch_secure_path_open_leaf(parent_fd, temp_name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
        if (fd != -1) {
            if (fchmod(fd, mode) == -1) {
                return writer_fail_temp_entry(errno, parent_fd, fd, &temp_name);
            }

            *temp_name_out = temp_name;
            return fd;
        }

        int error_number = errno;
        free(temp_name);
        if (error_number != EEXIST) {
            return writer_fail_errno(error_number);
        }
    }

    errno = EEXIST;
    return -1;
}

static int seed_temp_file_from_existing_destination(BxFetchWriter* w) {
    if (!w || w->fd == -1 || w->parent_fd == -1 || !w->basename) {
        errno = EINVAL;
        return -1;
    }

    int src_fd = bx_fetch_secure_path_open_leaf(w->parent_fd, w->basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (src_fd == -1) {
        if (errno == ENOENT) {
            w->written = 0;
            return 0;
        }
        return -1;
    }

    struct stat st;
    if (fstat(src_fd, &st) != 0) {
        return writer_fail_after_close(errno, src_fd);
    }
    if (!S_ISREG(st.st_mode) || !same_destination_identity(w, &st)) {
        close(src_fd);
        errno = S_ISREG(st.st_mode) ? EBUSY : EINVAL;
        return -1;
    }

    char buf[16384];
    while (true) {
        ssize_t nread = read(src_fd, buf, sizeof(buf));
        if (nread == 0)
            break;
        if (nread == -1) {
            if (errno == EINTR)
                continue;
            return writer_fail_after_close(errno, src_fd);
        }

        size_t written = 0;
        while (written < (size_t)nread) {
            ssize_t nwritten = write(w->fd, buf + written, (size_t)nread - written);
            if (nwritten == -1) {
                if (errno == EINTR)
                    continue;
                return writer_fail_after_close(errno, src_fd);
            }
            if (nwritten == 0) {
                return writer_fail_after_close(EIO, src_fd);
            }
            written += (size_t)nwritten;
        }
    }

    if (lseek(w->fd, 0, SEEK_END) == (off_t)-1) {
        return writer_fail_after_close(errno, src_fd);
    }

    w->written = (size_t)st.st_size;
    close(src_fd);
    return 0;
}

static int capture_destination_state_for_basename(BxFetchWriter* w, const char* basename) {
    if (!w || w->to_stdout || w->parent_fd == -1 || !basename) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (fstatat(w->parent_fd, basename, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            w->initial_dest_existed = false;
            w->initial_dest_dev = 0;
            w->initial_dest_ino = 0;
            w->initial_dest_mode = 0;
            w->initial_dest_mtime = 0;
            return 0;
        }
        return -1;
    }

    w->initial_dest_existed = true;
    w->initial_dest_dev = st.st_dev;
    w->initial_dest_ino = st.st_ino;
    w->initial_dest_mode = st.st_mode;
    w->initial_dest_mtime = st.st_mtime;
    return 0;
}

static int capture_destination_state(BxFetchWriter* w) {
    if (!w) {
        errno = EINVAL;
        return -1;
    }
    return capture_destination_state_for_basename(w, w->basename);
}

static bool same_destination_identity(const BxFetchWriter* w, const struct stat* st) {
    if (!w || !st || !w->initial_dest_existed)
        return false;
    return w->initial_dest_dev == st->st_dev && w->initial_dest_ino == st->st_ino && ((w->initial_dest_mode & S_IFMT) == (st->st_mode & S_IFMT));
}

static int copy_user_xattrs(int source_fd, int destination_fd) {
    const size_t max_xattr_bytes = 64u * 1024u;
    ssize_t list_size = flistxattr(source_fd, NULL, 0);
    if (list_size == -1) {
        if (errno == ENOTSUP || errno == EOPNOTSUPP)
            return 0;
        return -1;
    }
    if ((size_t)list_size > max_xattr_bytes) {
        errno = E2BIG;
        return -1;
    }
    if (list_size == 0)
        return 0;

    char* names = malloc((size_t)list_size);
    if (!names)
        return -1;
    ssize_t names_length = flistxattr(source_fd, names, (size_t)list_size);
    if (names_length == -1 || names_length > list_size) {
        int error_number = errno ? errno : EIO;
        free(names);
        errno = error_number;
        return -1;
    }

    size_t offset = 0;
    while (offset < (size_t)names_length) {
        size_t remaining = (size_t)names_length - offset;
        size_t name_length = strnlen(names + offset, remaining);
        if (name_length == remaining) {
            free(names);
            errno = EIO;
            return -1;
        }

        const char* name = names + offset;
        offset += name_length + 1u;
        if (strncmp(name, "user.", 5) != 0)
            continue;

        ssize_t value_size = fgetxattr(source_fd, name, NULL, 0);
        if (value_size == -1 || (size_t)value_size > max_xattr_bytes) {
            int error_number = value_size == -1 ? errno : E2BIG;
            free(names);
            errno = error_number;
            return -1;
        }

        void* value = NULL;
        if (value_size > 0) {
            value = malloc((size_t)value_size);
            if (!value) {
                free(names);
                return -1;
            }
            ssize_t read_size = fgetxattr(source_fd, name, value, (size_t)value_size);
            if (read_size != value_size) {
                int error_number = read_size == -1 ? errno : EIO;
                free(value);
                free(names);
                errno = error_number;
                return -1;
            }
        }

        if (fsetxattr(destination_fd, name, value, (size_t)value_size, 0) != 0) {
            int error_number = errno;
            free(value);
            free(names);
            errno = error_number;
            return -1;
        }
        free(value);
    }

    free(names);
    return 0;
}

static bool destination_requires_explicit_unlink(mode_t mode) {
    return S_ISREG(mode);
}

static int validate_unlink_replacement_target(const BxFetchWriter* w, bool* should_unlink) {
    if (!w || !should_unlink) {
        errno = EINVAL;
        return -1;
    }

    *should_unlink = false;
    if (!w->honor_unlink_on_commit || w->to_stdout || !w->temp_name) {
        return 0;
    }

    struct stat st;
    if (fstatat(w->parent_fd, w->basename, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
            return 0;
        }
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    if (!w->initial_dest_existed || !same_destination_identity(w, &st)) {
        errno = EBUSY;
        return -1;
    }

    if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
        errno = EPERM;
        return -1;
    }

    *should_unlink = destination_requires_explicit_unlink(st.st_mode);
    return 0;
}

static bool basename_is_simple_leaf(const char* basename) {
    return basename && basename[0] != '\0' && strcmp(basename, ".") != 0 && strcmp(basename, "..") != 0 && strchr(basename, '/') == NULL;
}

static int set_metadata_string(char** dest, const char* value) {
    char* copy = NULL;
    if (value && value[0] != '\0') {
        copy = strdup(value);
        if (!copy)
            return -1;
    }

    free(*dest);
    *dest = copy;
    return 0;
}

static int copy_metadata(BxFetchMetadata* dest, const BxFetchMetadata* src) {
    if (!dest || !src) {
        errno = EINVAL;
        return -1;
    }

    BxFetchMetadata copy = {0};
    if (set_metadata_string(&copy.etag, src->etag) != 0 || set_metadata_string(&copy.last_modified, src->last_modified) != 0 || set_metadata_string(&copy.origin_url, src->origin_url) != 0 ||
        set_metadata_string(&copy.redirect_target, src->redirect_target) != 0 || set_metadata_string(&copy.local_path, src->local_path) != 0) {
        bx_fetch_metadata_clear(&copy);
        return -1;
    }

    bx_fetch_metadata_clear(dest);
    *dest = copy;
    return 0;
}

static char* sidecar_name_for_basename(const char* basename) {
    if (!basename) {
        errno = EINVAL;
        return NULL;
    }

    char* sidecar = NULL;
    if (asprintf(&sidecar, "%s.mira.meta", basename) == -1) {
        return NULL;
    }
    return sidecar;
}

static int stream_flush_and_sync(FILE* f) {
    if (!f) {
        errno = EINVAL;
        return -1;
    }

    if (fflush(f) != 0)
        return -1;
    int fd = fileno(f);
    if (fd == -1)
        return -1;
    return fsync(fd);
}

static int write_metadata_temp_file_at(int parent_fd, const char* basename, const BxFetchMetadata* meta, char** temp_name_out) {
    if (parent_fd == -1 || !basename || !meta || !temp_name_out) {
        errno = EINVAL;
        return -1;
    }

    int fd = open_unique_temp_file_at(parent_fd, basename, temp_name_out);
    if (fd == -1)
        return -1;

    FILE* f = fdopen(fd, "w");
    if (!f) {
        return writer_fail_temp_entry(errno, parent_fd, fd, temp_name_out);
    }

    int rc = 0;
    if (bx_fetch_metadata_write_stream(f, meta) != 0) {
        rc = -1;
    }
    else if (stream_flush_and_sync(f) != 0) {
        rc = -1;
    }

    if (fclose(f) != 0) {
        rc = -1;
    }

    if (rc != 0) {
        return writer_fail_temp_entry(errno, parent_fd, -1, temp_name_out);
    }

    return 0;
}

static int rename_existing_entry_to_hold(int parent_fd, const char* name, char** hold_name_out) {
    if (parent_fd == -1 || !name || !hold_name_out) {
        errno = EINVAL;
        return -1;
    }

    *hold_name_out = NULL;

    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
        return (errno == ENOENT) ? 0 : -1;
    }

    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    static unsigned long long hold_counter = 0;
    for (unsigned long long attempt = 0; attempt < 128; attempt++) {
        char* hold_name = NULL;
        unsigned long long serial = (((unsigned long long)getpid()) << 32) | (hold_counter++);
        if (asprintf(&hold_name, "%s.mira.hold.%016llx%02llx", name, serial, attempt) == -1) {
            hold_name = NULL;
        }
        if (!hold_name)
            return -1;

        if (renameat(parent_fd, name, parent_fd, hold_name) == 0) {
            *hold_name_out = hold_name;
            return 0;
        }

        int error_number = errno;
        free(hold_name);
        if (error_number == ENOENT) {
            return 0;
        }
        if (error_number != EEXIST) {
            return writer_fail_errno(error_number);
        }
    }

    errno = EEXIST;
    return -1;
}

static void cleanup_temp_entry(int parent_fd, char** name) {
    if (parent_fd == -1 || !name || !*name)
        return;
    unlinkat(parent_fd, *name, 0);
    free(*name);
    *name = NULL;
}

static void restore_hold_entry(int parent_fd, char** hold_name, const char* final_name) {
    if (parent_fd == -1 || !hold_name || !*hold_name || !final_name)
        return;
    renameat(parent_fd, *hold_name, parent_fd, final_name);
    free(*hold_name);
    *hold_name = NULL;
}

static void writer_free(BxFetchWriter* w) {
    if (!w)
        return;
    if (w->parent_fd != -1)
        close(w->parent_fd);
    free(w->temp_name);
    free(w->superseded_sidecar_name);
    free(w->basename);
    free(w->parent_path);
    free(w->path);
    bx_fetch_metadata_clear(&w->pending_metadata);
    free(w);
}

static char* backup_name(const char* basename, int index) {
    size_t len = strlen(basename) + 32;
    char* out = malloc(len);
    if (!out)
        return NULL;
    snprintf(out, len, "%s.%d", basename, index);
    return out;
}

static bool parse_backup_index(const char* name, const char* basename, int* index_out) {
    if (!name || !basename || !index_out)
        return false;

    size_t basename_len = strlen(basename);
    if (strncmp(name, basename, basename_len) != 0 || name[basename_len] != '.') {
        return false;
    }

    const char* suffix = name + basename_len + 1;
    if (suffix[0] == '\0')
        return false;

    errno = 0;
    char* end = NULL;
    long parsed = strtol(suffix, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed <= 0 || parsed > INT_MAX) {
        return false;
    }

    *index_out = (int)parsed;
    return true;
}

static int prune_excess_backups_at(int parent_fd, const char* basename, int backups) {
    if (parent_fd == -1 || !basename || backups < 0) {
        errno = EINVAL;
        return -1;
    }

    int scan_fd = dup(parent_fd);
    if (scan_fd == -1)
        return -1;

    DIR* dir = fdopendir(scan_fd);
    if (!dir) {
        close(scan_fd);
        return -1;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        int index = 0;
        if (!parse_backup_index(ent->d_name, basename, &index) || index <= backups) {
            continue;
        }

        if (unlinkat(parent_fd, ent->d_name, 0) != 0 && errno != ENOENT) {
            int error_number = errno;
            closedir(dir);
            return writer_fail_errno(error_number);
        }
    }

    if (closedir(dir) != 0) {
        return -1;
    }

    return 0;
}

static int finalize_payload_hold(int parent_fd, const char* basename, int backups, bool rotate_backups_on_commit, char** hold_name_inout) {
    if (parent_fd == -1 || !basename || !hold_name_inout) {
        errno = EINVAL;
        return -1;
    }

    if (!*hold_name_inout)
        return 0;

    int rc = 0;
    if (rotate_backups_on_commit && backups > 0) {
        rc = prune_excess_backups_at(parent_fd, basename, backups);
        for (int i = backups; rc == 0 && i >= 2; i--) {
            char* src = backup_name(basename, i - 1);
            char* dst = backup_name(basename, i);
            if (!src || !dst) {
                free(src);
                free(dst);
                errno = ENOMEM;
                rc = -1;
                break;
            }

            if (renameat(parent_fd, src, parent_fd, dst) == -1 && errno != ENOENT) {
                rc = -1;
            }

            free(src);
            free(dst);
        }

        if (rc == 0) {
            char* dst = backup_name(basename, 1);
            if (!dst) {
                errno = ENOMEM;
                rc = -1;
            }
            else {
                if (renameat(parent_fd, *hold_name_inout, parent_fd, dst) != 0) {
                    rc = -1;
                }
                free(dst);
            }
        }
    }
    else if (unlinkat(parent_fd, *hold_name_inout, 0) != 0 && errno != ENOENT) {
        rc = -1;
    }

    free(*hold_name_inout);
    *hold_name_inout = NULL;
    return rc;
}

BxFetchWriter* bx_fetch_writer_open_with_options(const char* path, BxFetchWriterMode mode, int backups, bool unlink_existing) {
    if (!path)
        return NULL;

    BxFetchWriter* w = calloc(1, sizeof(BxFetchWriter));
    if (!w)
        return NULL;

    w->fd = -1;
    w->parent_fd = -1;
    w->path = strdup(path);
    if (!w->path) {
        writer_free(w);
        return NULL;
    }
    w->parent_path = parent_path_for_output_path(path);
    if (!w->parent_path) {
        writer_free(w);
        return NULL;
    }

    w->backups = (backups > 0) ? backups : 0;
    w->unlink_existing = unlink_existing;
    w->rotate_backups_on_commit = (mode == WRITER_CREATE);
    w->honor_unlink_on_commit = (mode == WRITER_CREATE) && unlink_existing;

    if (strcmp(path, "-") == 0) {
        w->to_stdout = true;
        w->fd = dup(STDOUT_FILENO);
        if (w->fd == -1) {
            writer_free(w);
            return NULL;
        }
        return w;
    }

    w->parent_fd = bx_fetch_secure_path_open_parent_directory(path, true, &w->basename);
    if (w->parent_fd == -1) {
        writer_free(w);
        return NULL;
    }

    if (capture_parent_directory_identity(w) != 0) {
        writer_free(w);
        return NULL;
    }

    if (capture_destination_state(w) != 0) {
        writer_free(w);
        return NULL;
    }

    w->fd = open_unique_temp_file_at(w->parent_fd, w->basename, &w->temp_name);
    if (w->fd == -1) {
        writer_free(w);
        return NULL;
    }

    if (mode == WRITER_RESUME) {
        if (seed_temp_file_from_existing_destination(w) != 0) {
            bx_fetch_writer_abort(w);
            return NULL;
        }
    }

    return w;
}

BxFetchWriter* bx_fetch_writer_open(const char* path, BxFetchWriterMode mode) {
    return bx_fetch_writer_open_with_options(path, mode, 0, false);
}

static int ensure_leaf_absent(int parent_fd, const char* name);

static int writer_set_final_path_with_policy(BxFetchWriter* w, const char* path, bool exclusive) {
    if (!w || !path || w->to_stdout) {
        errno = EINVAL;
        return -1;
    }

    char* new_parent = NULL;
    char* new_basename = NULL;
    if (bx_fetch_secure_path_split(path, &new_parent, &new_basename) != 0) {
        return -1;
    }

    if (!new_parent) {
        new_parent = strdup(".");
        if (!new_parent) {
            free(new_basename);
            return -1;
        }
    }

    if (!basename_is_simple_leaf(new_basename) || strcmp(new_parent, w->parent_path ? w->parent_path : ".") != 0) {
        free(new_parent);
        free(new_basename);
        errno = EXDEV;
        return -1;
    }

    if (strcmp(path, w->path) == 0) {
        if (exclusive) {
            char* sidecar = sidecar_name_for_basename(new_basename);
            if (!sidecar) {
                free(new_parent);
                free(new_basename);
                return -1;
            }
            if (ensure_leaf_absent(w->parent_fd, new_basename) != 0 || ensure_leaf_absent(w->parent_fd, sidecar) != 0) {
                int error_number = errno;
                free(sidecar);
                free(new_parent);
                free(new_basename);
                errno = error_number;
                return -1;
            }
            free(sidecar);
        }
        w->exclusive_final_path = exclusive;
        free(new_parent);
        free(new_basename);
        return 0;
    }

    if (exclusive) {
        char* new_sidecar = sidecar_name_for_basename(new_basename);
        if (!new_sidecar) {
            free(new_parent);
            free(new_basename);
            return -1;
        }
        if (ensure_leaf_absent(w->parent_fd, new_basename) != 0 || ensure_leaf_absent(w->parent_fd, new_sidecar) != 0) {
            int error_number = errno;
            free(new_sidecar);
            free(new_parent);
            free(new_basename);
            errno = error_number;
            return -1;
        }
        free(new_sidecar);
    }

    if (capture_destination_state_for_basename(w, new_basename) != 0) {
        free(new_parent);
        free(new_basename);
        return -1;
    }

    char* new_path = strdup(path);
    if (!new_path) {
        free(new_parent);
        free(new_basename);
        return -1;
    }

    char* old_sidecar = sidecar_name_for_basename(w->basename);
    if (!old_sidecar) {
        free(new_path);
        free(new_parent);
        free(new_basename);
        return -1;
    }

    free(w->superseded_sidecar_name);
    w->superseded_sidecar_name = old_sidecar;

    free(w->path);
    free(w->basename);
    w->path = new_path;
    w->basename = new_basename;
    w->exclusive_final_path = exclusive;

    free(new_parent);
    return 0;
}

int bx_fetch_writer_set_final_path(BxFetchWriter* w, const char* path) {
    return writer_set_final_path_with_policy(w, path, false);
}

int bx_fetch_writer_set_final_path_exclusive(BxFetchWriter* w, const char* path) {
    return writer_set_final_path_with_policy(w, path, true);
}

int bx_fetch_writer_stage_metadata(BxFetchWriter* w, const BxFetchMetadata* meta) {
    if (!w || w->to_stdout) {
        errno = EINVAL;
        return -1;
    }

    bx_fetch_metadata_clear(&w->pending_metadata);
    w->has_pending_metadata = false;
    if (!meta) {
        return 0;
    }

    if (copy_metadata(&w->pending_metadata, meta) != 0) {
        return -1;
    }

    w->has_pending_metadata = true;
    return 0;
}

int bx_fetch_writer_preserve_destination_metadata(BxFetchWriter* w) {
    if (!w || w->to_stdout || w->fd == -1 || w->parent_fd == -1) {
        errno = EINVAL;
        return -1;
    }
    if (!w->initial_dest_existed || !S_ISREG(w->initial_dest_mode)) {
        return 0;
    }

    int source_fd = bx_fetch_secure_path_open_leaf(w->parent_fd, w->basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (source_fd == -1)
        return -1;

    struct stat st;
    int rc = fstat(source_fd, &st);
    int error_number = errno;
    if (rc == 0 && !same_destination_identity(w, &st)) {
        rc = -1;
        error_number = EBUSY;
    }
    if (rc == 0 && fchmod(w->fd, st.st_mode & 07777) != 0) {
        rc = -1;
        error_number = errno;
    }
    if (rc == 0 && copy_user_xattrs(source_fd, w->fd) != 0) {
        rc = -1;
        error_number = errno;
    }
    if (close(source_fd) != 0 && rc == 0) {
        rc = -1;
        error_number = errno;
    }

    if (rc != 0)
        errno = error_number;
    return rc;
}

int bx_fetch_writer_begin_replace(BxFetchWriter* w) {
    if (!w)
        return -1;
    if (w->to_stdout)
        return -1;
    if (w->temp_name) {
        if (ftruncate(w->fd, 0) == -1) {
            return -1;
        }
        if (lseek(w->fd, 0, SEEK_SET) == (off_t)-1) {
            return -1;
        }
        w->written = 0;
        w->rotate_backups_on_commit = true;
        w->honor_unlink_on_commit = w->unlink_existing;
        return 0;
    }
    if (w->fd == -1)
        return -1;

    if (close(w->fd) == -1) {
        return -1;
    }
    w->fd = -1;

    w->fd = open_unique_temp_file_at(w->parent_fd, w->basename, &w->temp_name);
    if (w->fd == -1) {
        return -1;
    }

    w->written = 0;
    w->rotate_backups_on_commit = true;
    w->honor_unlink_on_commit = w->unlink_existing;
    return 0;
}

int bx_fetch_writer_write(BxFetchWriter* w, const void* data, size_t len) {
    if (!w || w->fd == -1 || (!data && len > 0))
        return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t n = write(w->fd, (const char*)data + written, len - written);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        written += (size_t)n;
    }
    return 0;
}

int bx_fetch_writer_set_mtime(BxFetchWriter* w, time_t mtime) {
    if (!w || w->to_stdout || w->fd == -1) {
        errno = EINVAL;
        return -1;
    }

    w->pending_mtime = mtime;
    w->has_pending_mtime = true;
    return 0;
}

int bx_fetch_writer_stage_xattrs(BxFetchWriter* w, const char* url, const char* content_type, const char* etag, const char* last_modified) {
    if (!w || w->to_stdout || w->fd == -1) {
        errno = EINVAL;
        return BX_FETCH_XATTR_ERROR;
    }
    return bx_fetch_xattr_apply_fd(w->fd, url, content_type, etag, last_modified);
}

static int ensure_leaf_absent(int parent_fd, const char* name) {
    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        return -1;
    }
    return errno == ENOENT ? 0 : -1;
}

static void unlink_leaf_if_same_identity(int parent_fd, const char* name, const struct stat* expected) {
    if (parent_fd == -1 || !name || !expected)
        return;

    struct stat current;
    if (fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0) {
        return;
    }
    if (current.st_dev == expected->st_dev && current.st_ino == expected->st_ino && (current.st_mode & S_IFMT) == (expected->st_mode & S_IFMT)) {
        (void)unlinkat(parent_fd, name, 0);
    }
}

int bx_fetch_writer_close(BxFetchWriter* w) {
    if (!w)
        return -1;

    if (w->to_stdout) {
        int rc = 0;
        if (w->fd != -1 && close(w->fd) == -1) {
            rc = -1;
        }
        w->fd = -1;
        writer_free(w);
        return rc;
    }

    bool should_fsync = true;
    struct stat st;
    if (fstat(w->fd, &st) == 0 && !S_ISREG(st.st_mode)) {
        should_fsync = false;
    }

    if (w->has_pending_mtime) {
        struct timespec times[2] = {
            {.tv_sec = 0, .tv_nsec = UTIME_OMIT},
            {.tv_sec = w->pending_mtime, .tv_nsec = 0},
        };
        if (futimens(w->fd, times) != 0) {
            if (w->fd != -1)
                close(w->fd);
            if (w->temp_name)
                unlinkat(w->parent_fd, w->temp_name, 0);
            writer_free(w);
            return -1;
        }
    }

    if (should_fsync && fsync(w->fd) == -1) {
        if (w->fd != -1)
            close(w->fd);
        if (w->temp_name)
            unlinkat(w->parent_fd, w->temp_name, 0);
        writer_free(w);
        return -1;
    }

    if (close(w->fd) == -1) {
        if (w->temp_name)
            unlinkat(w->parent_fd, w->temp_name, 0);
        writer_free(w);
        return -1;
    }
    w->fd = -1;

    if (validate_parent_directory_identity(w) != 0) {
        cleanup_temp_entry(w->parent_fd, &w->temp_name);
        writer_free(w);
        return -1;
    }

    int rc = 0;
    char* sidecar_name = NULL;
    char* sidecar_temp_name = NULL;
    char* payload_hold_name = NULL;
    char* sidecar_hold_name = NULL;
    bool sidecar_committed = false;
    struct stat sidecar_candidate_stat = {0};
    bool have_sidecar_candidate_stat = false;

    if (w->temp_name) {
        bool should_unlink = false;
        if (validate_unlink_replacement_target(w, &should_unlink) != 0) {
            cleanup_temp_entry(w->parent_fd, &w->temp_name);
            writer_free(w);
            return -1;
        }
        (void)should_unlink;

        if (w->has_pending_metadata) {
            sidecar_name = sidecar_name_for_basename(w->basename);
            if (!sidecar_name) {
                cleanup_temp_entry(w->parent_fd, &w->temp_name);
                writer_free(w);
                return -1;
            }

            if (!bx_fetch_metadata_is_empty(&w->pending_metadata) && write_metadata_temp_file_at(w->parent_fd, sidecar_name, &w->pending_metadata, &sidecar_temp_name) != 0) {
                free(sidecar_name);
                cleanup_temp_entry(w->parent_fd, &w->temp_name);
                writer_free(w);
                return -1;
            }
        }
        if (w->exclusive_final_path && !sidecar_name) {
            sidecar_name = sidecar_name_for_basename(w->basename);
            if (!sidecar_name) {
                cleanup_temp_entry(w->parent_fd, &w->temp_name);
                writer_free(w);
                return -1;
            }
        }

        if (!w->exclusive_final_path && rename_existing_entry_to_hold(w->parent_fd, w->basename, &payload_hold_name) != 0) {
            free(sidecar_name);
            cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
            cleanup_temp_entry(w->parent_fd, &w->temp_name);
            writer_free(w);
            return -1;
        }

        if (!w->exclusive_final_path && sidecar_name && rename_existing_entry_to_hold(w->parent_fd, sidecar_name, &sidecar_hold_name) != 0) {
            restore_hold_entry(w->parent_fd, &payload_hold_name, w->basename);
            free(sidecar_name);
            cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
            cleanup_temp_entry(w->parent_fd, &w->temp_name);
            writer_free(w);
            return -1;
        }

        if (w->exclusive_final_path && sidecar_name && !sidecar_temp_name && ensure_leaf_absent(w->parent_fd, sidecar_name) != 0) {
            free(sidecar_name);
            cleanup_temp_entry(w->parent_fd, &w->temp_name);
            writer_free(w);
            return -1;
        }

        if (sidecar_temp_name) {
            if (fstatat(w->parent_fd, sidecar_temp_name, &sidecar_candidate_stat, AT_SYMLINK_NOFOLLOW) == 0) {
                have_sidecar_candidate_stat = true;
            }
            else {
                restore_hold_entry(w->parent_fd, &sidecar_hold_name, sidecar_name);
                restore_hold_entry(w->parent_fd, &payload_hold_name, w->basename);
                free(sidecar_name);
                cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
                cleanup_temp_entry(w->parent_fd, &w->temp_name);
                writer_free(w);
                return -1;
            }

            int sidecar_rename_rc = w->exclusive_final_path ? bx_fetch_secure_path_rename_leaf_noreplace(w->parent_fd, sidecar_temp_name, sidecar_name)
                                                            : renameat(w->parent_fd, sidecar_temp_name, w->parent_fd, sidecar_name);
            if (sidecar_rename_rc != 0) {
                restore_hold_entry(w->parent_fd, &sidecar_hold_name, sidecar_name);
                restore_hold_entry(w->parent_fd, &payload_hold_name, w->basename);
                free(sidecar_name);
                cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
                cleanup_temp_entry(w->parent_fd, &w->temp_name);
                writer_free(w);
                return -1;
            }

            free(sidecar_temp_name);
            sidecar_temp_name = NULL;
            sidecar_committed = true;
        }

        int payload_rename_rc =
            w->exclusive_final_path ? bx_fetch_secure_path_rename_leaf_noreplace(w->parent_fd, w->temp_name, w->basename) : renameat(w->parent_fd, w->temp_name, w->parent_fd, w->basename);
        if (payload_rename_rc != 0) {
            int error_number = errno;
            if (sidecar_committed && sidecar_name && have_sidecar_candidate_stat) {
                unlink_leaf_if_same_identity(w->parent_fd, sidecar_name, &sidecar_candidate_stat);
            }
            restore_hold_entry(w->parent_fd, &sidecar_hold_name, sidecar_name);
            restore_hold_entry(w->parent_fd, &payload_hold_name, w->basename);
            free(sidecar_name);
            cleanup_temp_entry(w->parent_fd, &w->temp_name);
            writer_free(w);
            errno = error_number;
            return -1;
        }

        free(w->temp_name);
        w->temp_name = NULL;

        if (fsync(w->parent_fd) == -1) {
            rc = -1;
        }

        if (finalize_payload_hold(w->parent_fd, w->basename, w->backups, w->rotate_backups_on_commit, &payload_hold_name) != 0) {
            rc = -1;
        }

        if (sidecar_hold_name) {
            if (unlinkat(w->parent_fd, sidecar_hold_name, 0) != 0 && errno != ENOENT) {
                rc = -1;
            }
            free(sidecar_hold_name);
            sidecar_hold_name = NULL;
        }

        if (w->superseded_sidecar_name) {
            if (unlinkat(w->parent_fd, w->superseded_sidecar_name, 0) != 0 && errno != ENOENT) {
                rc = -1;
            }
        }

        if (fsync(w->parent_fd) == -1) {
            rc = -1;
        }
    }

    free(sidecar_name);
    writer_free(w);
    return rc;
}

static bool current_destination_matches(const BxFetchWriter* w) {
    if (!w || w->parent_fd == -1 || !w->basename || !w->initial_dest_existed)
        return false;

    struct stat st;
    if (fstatat(w->parent_fd, w->basename, &st, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    if (!S_ISREG(st.st_mode) || !same_destination_identity(w, &st)) {
        errno = EBUSY;
        return false;
    }
    return true;
}

static bool metadata_candidate_is_published(const BxFetchWriter* w, const char* sidecar_name, const struct stat* candidate_stat) {
    if (!w || !sidecar_name || !candidate_stat)
        return false;

    struct stat current;
    if (fstatat(w->parent_fd, sidecar_name, &current, AT_SYMLINK_NOFOLLOW) != 0)
        return false;
    return current.st_dev == candidate_stat->st_dev && current.st_ino == candidate_stat->st_ino && S_ISREG(current.st_mode);
}

static void rollback_metadata_exchange(BxFetchWriter* w, const char* sidecar_name, char** sidecar_temp_name, bool sidecar_existed, const struct stat* candidate_stat) {
    if (!w || !sidecar_name || !sidecar_temp_name || !*sidecar_temp_name || !candidate_stat)
        return;

    if (sidecar_existed) {
        if (metadata_candidate_is_published(w, sidecar_name, candidate_stat) && bx_fetch_secure_path_exchange_leaves(w->parent_fd, sidecar_name, *sidecar_temp_name) == 0) {
            cleanup_temp_entry(w->parent_fd, sidecar_temp_name);
        }
    }
    else {
        unlink_leaf_if_same_identity(w->parent_fd, sidecar_name, candidate_stat);
    }
    (void)fsync(w->parent_fd);
}

BxFetchWriterMetadataCommitResult bx_fetch_writer_close_metadata_only(BxFetchWriter* w) {
    if (!w) {
        errno = EINVAL;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    if (w->to_stdout || w->fd == -1 || w->parent_fd == -1 || !w->temp_name || w->has_pending_mtime || w->superseded_sidecar_name || w->exclusive_final_path) {
        bx_fetch_writer_abort(w);
        errno = EINVAL;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    if (!w->has_pending_metadata) {
        bx_fetch_writer_abort(w);
        return BX_FETCH_WRITER_METADATA_UNCHANGED;
    }
    if (!w->initial_dest_existed || !S_ISREG(w->initial_dest_mode)) {
        bx_fetch_writer_abort(w);
        errno = ENOENT;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    int original_fd = bx_fetch_secure_path_open_leaf(w->parent_fd, w->basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    if (original_fd == -1) {
        int error_number = errno;
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    struct stat original_stat;
    if (fstat(original_fd, &original_stat) != 0) {
        int error_number = errno;
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    if (!S_ISREG(original_stat.st_mode) || !same_destination_identity(w, &original_stat)) {
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = EBUSY;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    if (validate_parent_directory_identity(w) != 0) {
        int error_number = errno;
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    char* sidecar_name = sidecar_name_for_basename(w->basename);
    char* sidecar_temp_name = NULL;
    if (!sidecar_name || bx_fetch_metadata_is_empty(&w->pending_metadata) || write_metadata_temp_file_at(w->parent_fd, sidecar_name, &w->pending_metadata, &sidecar_temp_name) != 0) {
        int error_number = errno ? errno : EINVAL;
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    struct stat candidate_stat;
    if (fstatat(w->parent_fd, sidecar_temp_name, &candidate_stat, AT_SYMLINK_NOFOLLOW) != 0 || !current_destination_matches(w)) {
        int error_number = errno ? errno : EBUSY;
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    struct stat prior_sidecar_stat;
    bool sidecar_existed = fstatat(w->parent_fd, sidecar_name, &prior_sidecar_stat, AT_SYMLINK_NOFOLLOW) == 0;
    if (!sidecar_existed && errno != ENOENT) {
        int error_number = errno;
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    if (sidecar_existed && S_ISDIR(prior_sidecar_stat.st_mode)) {
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = EISDIR;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    if (close(w->fd) != 0) {
        int error_number = errno;
        w->fd = -1;
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        bx_fetch_writer_abort(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    w->fd = -1;
    if (unlinkat(w->parent_fd, w->temp_name, 0) != 0) {
        int error_number = errno;
        free(sidecar_name);
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        close(original_fd);
        writer_free(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    free(w->temp_name);
    w->temp_name = NULL;

    if (!current_destination_matches(w)) {
        int error_number = errno ? errno : EIO;
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        free(sidecar_name);
        close(original_fd);
        writer_free(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    int promote_result = sidecar_existed ? bx_fetch_secure_path_exchange_leaves(w->parent_fd, sidecar_temp_name, sidecar_name)
                                         : bx_fetch_secure_path_rename_leaf_noreplace(w->parent_fd, sidecar_temp_name, sidecar_name);
    if (promote_result != 0) {
        int error_number = errno;
        cleanup_temp_entry(w->parent_fd, &sidecar_temp_name);
        free(sidecar_name);
        close(original_fd);
        writer_free(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    if (!metadata_candidate_is_published(w, sidecar_name, &candidate_stat) || !current_destination_matches(w) || fsync(w->parent_fd) != 0) {
        int error_number = errno ? errno : EIO;
        rollback_metadata_exchange(w, sidecar_name, &sidecar_temp_name, sidecar_existed, &candidate_stat);
        free(sidecar_temp_name);
        free(sidecar_name);
        close(original_fd);
        writer_free(w);
        errno = error_number;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }

    int rc = 0;
    if (sidecar_existed && unlinkat(w->parent_fd, sidecar_temp_name, 0) != 0 && errno != ENOENT)
        rc = -1;
    free(sidecar_temp_name);
    if (fsync(w->parent_fd) != 0)
        rc = -1;

    int error_number = errno;
    free(sidecar_name);
    close(original_fd);
    writer_free(w);
    if (rc != 0) {
        errno = error_number ? error_number : EIO;
        return BX_FETCH_WRITER_METADATA_COMMIT_ERROR;
    }
    return BX_FETCH_WRITER_METADATA_COMMITTED;
}

void bx_fetch_writer_abort(BxFetchWriter* w) {
    if (!w)
        return;

    if (w->fd != -1)
        close(w->fd);
    if (w->temp_name)
        unlinkat(w->parent_fd, w->temp_name, 0);

    writer_free(w);
}

uint64_t bx_fetch_writer_candidate_size(const BxFetchWriter* w) {
    return w ? (uint64_t)w->written : 0;
}

bool bx_fetch_writer_original_mtime(const BxFetchWriter* w, time_t* mtime_out) {
    if (!w || !mtime_out || !w->initial_dest_existed || !S_ISREG(w->initial_dest_mode)) {
        return false;
    }
    *mtime_out = w->initial_dest_mtime;
    return true;
}

int bx_fetch_writer_load_original_metadata(const BxFetchWriter* w, BxFetchMetadata* metadata) {
    if (!w || !metadata || w->to_stdout || w->parent_fd == -1 || !w->basename) {
        errno = EINVAL;
        return -1;
    }
    bx_fetch_metadata_clear(metadata);

    char* sidecar_name = sidecar_name_for_basename(w->basename);
    if (!sidecar_name)
        return -1;
    int fd = openat(w->parent_fd, sidecar_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int open_error = errno;
    free(sidecar_name);
    if (fd == -1) {
        errno = open_error;
        return open_error == ENOENT ? 0 : -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int error_number = errno;
        close(fd);
        errno = error_number;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    FILE* stream = fdopen(fd, "r");
    if (!stream) {
        int error_number = errno;
        close(fd);
        errno = error_number;
        return -1;
    }
    int result = bx_fetch_metadata_read_stream(stream, metadata);
    int error_number = errno;
    if (fclose(stream) != 0 && result == 0) {
        result = -1;
        error_number = errno;
    }
    if (result != 0)
        errno = error_number;
    return result;
}

const char* bx_fetch_writer_get_path(const BxFetchWriter* w) {
    if (!w)
        return NULL;
    return w->path;
}
