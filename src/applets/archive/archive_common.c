#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "applets/archive/archive_temp.h"
#include "bx/libbx.h"

#define BX_ARCHIVE_FILE_STREAM_BUFFER_SIZE (1024u * 1024u)
#include "lib/fd_ops.h"
#include "lib/checked_math.h"
#include "lib/mode_parse.h"
#include "lib/path_ops.h"
#include "lib/xreadwrite.h"

static bool bx_archive_buffer_reserve(struct bx_archive_buffer* buffer, size_t extra) {
    size_t need;
    size_t next_cap;

    if (extra == 0u) {
        return true;
    }
    if (buffer->len > SIZE_MAX - extra) {
        errno = EOVERFLOW;
        return false;
    }

    need = buffer->len + extra;
    if (need <= buffer->cap) {
        return true;
    }

    next_cap = buffer->cap ? buffer->cap : 4096u;
    while (next_cap < need) {
        if (next_cap > SIZE_MAX / 2u) {
            next_cap = need;
            break;
        }
        next_cap *= 2u;
    }

    buffer->data = xrealloc(buffer->data, next_cap);
    buffer->cap = next_cap;
    return true;
}

void bx_archive_buffer_init(struct bx_archive_buffer* buffer) {
    buffer->data = NULL;
    buffer->len = 0u;
    buffer->cap = 0u;
}

void bx_archive_buffer_free(struct bx_archive_buffer* buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->len = 0u;
    buffer->cap = 0u;
}

bool bx_archive_buffer_append(struct bx_archive_buffer* buffer, const void* data, size_t len) {
    if (len == 0u) {
        return true;
    }
    if (!bx_archive_buffer_reserve(buffer, len)) {
        return false;
    }
    memcpy(buffer->data + buffer->len, data, len);
    buffer->len += len;
    return true;
}

bool bx_archive_buffer_append_byte(struct bx_archive_buffer* buffer, unsigned char value) {
    return bx_archive_buffer_append(buffer, &value, 1u);
}

bool bx_archive_buffer_append_zeros(struct bx_archive_buffer* buffer, size_t len) {
    if (!bx_archive_buffer_reserve(buffer, len)) {
        return false;
    }
    memset(buffer->data + buffer->len, 0, len);
    buffer->len += len;
    return true;
}

bool bx_archive_buffer_read_all(int fd, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag) {
    unsigned char chunk[8192];
    struct bx_fd_input input = BX_FD_INPUT_INIT;
    bool ok = false;
    if (bx_fd_input_init(&input, fd, BX_FD_INPUT_BORROWED) != 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }

    while (true) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "read error: %s", strerror(errno));
            break;
        }
        ssize_t nread = bx_fd_input_read(&input, chunk, sizeof(chunk), bx_archive_temp_signal_fd());
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            nread = -1;
        }
        if (nread < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            break;
        }
        if (nread > 0 && !bx_archive_buffer_append(buffer, chunk, (size_t)nread)) {
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            break;
        }
        if (nread == 0) {
            ok = true;
            break;
        }
    }
    bx_fd_input_close(&input);
    return ok;
}

bool bx_archive_buffer_write_all(FILE* stream, const struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag) {
    if (buffer->len != 0u && fwrite(buffer->data, 1u, buffer->len, stream) != buffer->len) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (fflush(stream) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

bool bx_archive_buffer_has_gzip_magic(const struct bx_archive_buffer* buffer) {
    return buffer != NULL
        && buffer->len >= 2u
        && buffer->data[0] == 0x1fu
        && buffer->data[1] == 0x8bu;
}

void bx_archive_name_list_free(struct bx_archive_name_list* list) {
    size_t i;

    for (i = 0u; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0u;
    list->cap = 0u;
}

bool bx_archive_name_list_append(struct bx_archive_name_list* list, const char* name) {
    char** next_items;

    if (list->len == list->cap) {
        size_t maximum = SIZE_MAX / sizeof(*list->items);
        if (list->cap >= maximum) {
            errno = EOVERFLOW;
            return false;
        }
        size_t next_cap = list->cap ? (list->cap > maximum / 2u ? maximum : list->cap * 2u) : 16u;
        next_items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->items = next_items;
        list->cap = next_cap;
    }

    list->items[list->len++] = xstrdup(name);
    return true;
}

bool bx_archive_name_list_read_fd_bounded(int fd, unsigned char separator, struct bx_archive_name_list* list, size_t count_limit, size_t input_byte_limit, struct bx_diag_ctx* diag) {
    struct bx_fd_input input = BX_FD_INPUT_INIT;
    struct bx_archive_buffer name = {0};
    unsigned char chunk[8192];
    size_t remaining = input_byte_limit;
    bool ok = false;
    if (list->len > count_limit)
        goto limit_error;
    if (bx_fd_input_init(&input, fd, BX_FD_INPUT_BORROWED) != 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    while (true) {
        size_t request = remaining < sizeof(chunk) ? remaining + 1u : sizeof(chunk);
        ssize_t nread;
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            goto read_error;
        }
        nread = bx_fd_input_read(&input, chunk, request, bx_archive_temp_signal_fd());
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            goto read_error;
        }
        if (nread < 0)
            goto read_error;
        if ((size_t)nread > remaining)
            goto limit_error;
        remaining -= (size_t)nread;
        size_t start = 0;
        for (size_t i = 0; i <= (size_t)nread; i++) {
            bool at_end = i == (size_t)nread;
            if (!at_end && chunk[i] != separator)
                continue;
            size_t length = i - start;
            if (length > 0) {
                if (list->len >= count_limit)
                    goto limit_error;
                if (separator != '\0' && memchr(chunk + start, '\0', length)) {
                    errno = EINVAL;
                    bx_diag(diag, "NUL byte in newline-delimited name list");
                    goto done;
                }
                if (!bx_archive_buffer_append(&name, chunk + start, length))
                    goto growth_error;
            }
            if ((!at_end || nread == 0) && name.len > 0) {
                if (!bx_archive_buffer_append_byte(&name, '\0') || !bx_archive_name_list_append(list, (const char*)name.data))
                    goto growth_error;
                name.len = 0;
            }
            start = i + 1u;
        }
        if (nread == 0) {
            ok = true;
            goto done;
        }
    }
read_error:
    bx_diag(diag, "read error: %s", strerror(errno));
    goto done;
limit_error:
    errno = E2BIG;
    bx_diag(diag, "name list limit exceeded");
    goto done;
growth_error:
    bx_diag(diag, "buffer growth failed: %s", strerror(errno));
done:
    bx_archive_buffer_free(&name);
    bx_fd_input_close(&input);
    return ok;
}

bool bx_archive_name_list_read_fd(int fd, unsigned char separator, struct bx_archive_name_list* list, struct bx_diag_ctx* diag) {
    return bx_archive_name_list_read_fd_bounded(fd, separator, list, SIZE_MAX / sizeof(*list->items), SIZE_MAX, diag);
}

bool bx_archive_name_list_read_path(const char* path,
                                    unsigned char separator,
                                    struct bx_archive_name_list* list,
                                    struct bx_diag_ctx* diag) {
    int fd;
    bool ok;

    if (strcmp(path, "-") == 0) {
        return bx_archive_name_list_read_fd(STDIN_FILENO, separator, list, diag);
    }

    fd = bx_fd_open_cloexec(path, O_RDONLY, 0);
    if (fd < 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }

    ok = bx_archive_name_list_read_fd(fd, separator, list, diag);
    if (close(fd) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return ok;
}

static bool bx_archive_write_payload_bytes(int fd, const unsigned char* data, size_t len) {
    while (len) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            return false;
        }
        size_t chunk = len < 65536u ? len : 65536u;
        ssize_t written = write(fd, data, chunk);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0) {
            errno = EIO;
            return false;
        }
        data += written;
        len -= (size_t)written;
    }
    return true;
}

static bool bx_archive_write_payload_span(int fd, const unsigned char* data, size_t len, bool sparse, bool* used_sparse, struct bx_diag_ctx* diag) {
    size_t offset = 0u;

    if (!sparse) {
        if (!bx_archive_write_payload_bytes(fd, data, len)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        return true;
    }

    while (offset < len) {
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        size_t span = 0u;

        if (data[offset] == 0u) {
            while (span < 65536u && offset + span < len && data[offset + span] == 0u) {
                span++;
            }
            if (lseek(fd, (off_t)span, SEEK_CUR) < 0) {
                bx_diag(diag, "write error: %s", strerror(errno));
                return false;
            }
            offset += span;
            *used_sparse = true;
            continue;
        }

        while (span < 65536u && offset + span < len && data[offset + span] != 0u) {
            span++;
        }
        if (!bx_archive_write_payload_bytes(fd, data + offset, span)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        offset += span;
    }
    return true;
}

static bool bx_archive_finish_payload(int fd, off_t logical_end, bool used_sparse, struct bx_diag_ctx* diag) {
    if (used_sparse && ftruncate(fd, logical_end) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    return true;
}

bool bx_archive_write_regular_payload(int fd, const unsigned char* data, size_t len, bool sparse, struct bx_diag_ctx* diag) {
    bool used_sparse = false;
    if (sparse && ((off_t)len < 0 || (uintmax_t)(off_t)len != len)) {
        bx_diag(diag, "write error: %s", strerror(EOVERFLOW));
        return false;
    }
    return bx_archive_write_payload_span(fd, data, len, sparse, &used_sparse, diag) && bx_archive_finish_payload(fd, sparse ? (off_t)len : 0, used_sparse, diag);
}

bool bx_archive_copy_regular_payload(int source_fd, int fd, off_t limit, bool sparse, off_t* copied, struct bx_diag_ctx* diag) {
    struct bx_fd_input input = BX_FD_INPUT_INIT;
    unsigned char chunk[65536];
    bool used_sparse = false;
    bool ok = false;
    *copied = 0;
    if (limit < 0 || bx_fd_input_init(&input, source_fd, BX_FD_INPUT_BORROWED) != 0) {
        bx_diag(diag, "read error: %s", strerror(limit < 0 ? EOVERFLOW : errno));
        return false;
    }
    while (*copied < limit) {
        off_t remaining = limit - *copied;
        size_t length = remaining < (off_t)sizeof(chunk) ? (size_t)remaining : sizeof(chunk);
        ssize_t nread;
        if (bx_archive_temp_pending_signal()) {
            errno = EINTR;
            nread = -1;
        }
        else {
            nread = bx_fd_input_read(&input, chunk, length, bx_archive_temp_signal_fd());
            if (bx_archive_temp_pending_signal()) {
                errno = EINTR;
                nread = -1;
            }
        }
        if (nread < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            goto done;
        }
        if (nread == 0)
            break;
        if (!bx_archive_write_payload_span(fd, chunk, (size_t)nread, sparse, &used_sparse, diag))
            goto done;
        *copied += nread;
    }
    ok = bx_archive_finish_payload(fd, *copied, used_sparse, diag);
done:
    bx_fd_input_close(&input);
    return ok;
}

static bool bx_archive_spool_end(const struct bx_archive_spool* spool, uintmax_t extra, off_t* end) {
    uintmax_t next;
    if (!bx_checked_uintmax_add(spool->len, extra, &next) || (off_t)next < 0 || (uintmax_t)(off_t)next != next) {
        errno = EOVERFLOW;
        return false;
    }
    *end = (off_t)next;
    return true;
}

bool bx_archive_spool_open(struct bx_archive_spool* spool, struct bx_diag_ctx* diag) {
    if (spool->fd >= 0) {
        errno = EALREADY;
        bx_diag(diag, "temporary archive: %s", strerror(errno));
        return false;
    }
    const char* directory = getenv("TMPDIR");
    if (!directory || !*directory)
        directory = "/tmp";
    *spool = (struct bx_archive_spool)BX_ARCHIVE_SPOOL_INIT;
    int parent = bx_fd_open_cloexec(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0);
    if (parent >= 0)
        spool->fd = bx_fd_open_anonymous_file_at(parent);
    int error = errno;
    if (parent >= 0 && close(parent) != 0 && spool->fd >= 0) {
        error = errno;
        bx_fd_cleanup(&spool->fd);
    }
    if (spool->fd < 0) {
        bx_diag(diag, "temporary archive: %s", strerror(error));
        return false;
    }
    return true;
}

bool bx_archive_spool_close(struct bx_archive_spool* spool, struct bx_diag_ctx* diag) {
    spool->len = 0;
    return bx_fd_close(&spool->fd, "temporary archive", diag);
}

bool bx_archive_spool_append(struct bx_archive_spool* spool, const void* data, size_t len) {
    off_t end;
    if (!bx_archive_spool_end(spool, len, &end) || !bx_archive_write_payload_bytes(spool->fd, data, len))
        return false;
    spool->len = (uintmax_t)end;
    return true;
}

bool bx_archive_spool_append_zeros(struct bx_archive_spool* spool, size_t len) {
    static const unsigned char zeros[4096];
    off_t end;
    if (!bx_archive_spool_end(spool, len, &end))
        return false;
    while (len) {
        size_t count = len < sizeof(zeros) ? len : sizeof(zeros);
        if (!bx_archive_spool_append(spool, zeros, count))
            return false;
        len -= count;
    }
    return true;
}

bool bx_archive_spool_copy(struct bx_archive_spool* spool, int source_fd, off_t length, off_t* copied, struct bx_diag_ctx* diag) {
    off_t end;
    *copied = 0;
    if (length < 0 || !bx_archive_spool_end(spool, (uintmax_t)length, &end)) {
        bx_diag(diag, "temporary archive: %s", strerror(EOVERFLOW));
        return false;
    }
    if (!bx_archive_copy_regular_payload(source_fd, spool->fd, length, false, copied, diag))
        return false;
    spool->len += (uintmax_t)*copied;
    return true;
}

bool bx_archive_spool_write_all(FILE* stream, const struct bx_archive_spool* spool, struct bx_diag_ctx* diag) {
    if (fflush(stream) != 0 || fileno(stream) < 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        return false;
    }
    if (bx_fd_lseek(spool->fd, 0, SEEK_SET) < 0) {
        bx_diag(diag, "read error: %s", strerror(errno));
        return false;
    }
    off_t copied;
    if (!bx_archive_copy_regular_payload(spool->fd, fileno(stream), (off_t)spool->len, false, &copied, diag))
        return false;
    if ((uintmax_t)copied != spool->len) {
        bx_diag(diag, "read error: unexpected end of temporary archive");
        return false;
    }
    return true;
}

static bool bx_archive_output_file_open_direct(struct bx_archive_output_file* out,
                                               const char* archive_path,
                                               struct bx_diag_ctx* diag) {
    out->stream = fopen(archive_path, "wb");
    if (out->stream == NULL) {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        return false;
    }
    setvbuf(out->stream, NULL, _IOFBF, BX_ARCHIVE_FILE_STREAM_BUFFER_SIZE);
    out->display_path = archive_path;
    return true;
}

enum bx_archive_output_stage_result {
    BX_ARCHIVE_OUTPUT_STAGE_READY = 0,
    BX_ARCHIVE_OUTPUT_STAGE_BYPASS,
    BX_ARCHIVE_OUTPUT_STAGE_ERROR,
};

static enum bx_archive_output_stage_result bx_archive_output_file_try_stage(
    struct bx_archive_output_file* out,
    const char* archive_path,
    struct bx_diag_ctx* diag) {
    struct stat path_lstat;
    struct stat target_stat;
    bool target_exists = false;
    char* publish_path = xstrdup(archive_path);
    char* target_dir = NULL;
    char* temp_path = NULL;
    FILE* stream = NULL;
    int fd = -1;
    mode_t mode_bits;
    bool ok = false;
    enum bx_archive_output_stage_result result = BX_ARCHIVE_OUTPUT_STAGE_ERROR;

    if (lstat(archive_path, &path_lstat) == 0 && S_ISLNK(path_lstat.st_mode)) {
        free(publish_path);
        publish_path = bx_path_realpath_dup(archive_path);
        if (publish_path == NULL) {
            result = BX_ARCHIVE_OUTPUT_STAGE_BYPASS;
            goto out;
        }
    }

    if (stat(publish_path, &target_stat) == 0) {
        target_exists = true;
        if (!S_ISREG(target_stat.st_mode)) {
            result = BX_ARCHIVE_OUTPUT_STAGE_BYPASS;
            goto out;
        }
        mode_bits = target_stat.st_mode & 07777u;
    }
    else if (errno == ENOENT) {
        mode_bits = 0666u & ~bx_mode_current_umask();
    }
    else {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        goto out;
    }

    target_dir = bx_path_dirname_dup(publish_path);
    temp_path = bx_path_join(target_dir, ".bx-archive-stage.XXXXXX");
    fd = mkstemp(temp_path);
    if (fd < 0) {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        goto out;
    }
    if (!bx_archive_temp_track(temp_path)) {
        bx_diag(diag, "failed to track temporary archive path");
        goto out;
    }
    if (fchmod(fd, mode_bits) != 0) {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        goto out;
    }
    if (target_exists && fchown(fd, target_stat.st_uid, target_stat.st_gid) != 0) {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        goto out;
    }
    stream = fdopen(fd, "wb");
    if (stream == NULL) {
        bx_diag(diag, "%s: %s", archive_path, strerror(errno));
        goto out;
    }
    setvbuf(stream, NULL, _IOFBF, BX_ARCHIVE_FILE_STREAM_BUFFER_SIZE);
    fd = -1;

    out->stream = stream;
    out->publish_path = publish_path;
    out->temp_path = temp_path;
    out->display_path = archive_path;
    out->transactional = true;
    stream = NULL;
    publish_path = NULL;
    temp_path = NULL;
    ok = true;
    result = BX_ARCHIVE_OUTPUT_STAGE_READY;

out:
    if (fd >= 0) {
        close(fd);
    }
    if (stream != NULL) {
        fclose(stream);
    }
    if (temp_path != NULL) {
        unlink(temp_path);
        bx_archive_temp_untrack(temp_path);
    }
    free(temp_path);
    free(target_dir);
    free(publish_path);
    if (!ok && result == BX_ARCHIVE_OUTPUT_STAGE_READY) {
        result = BX_ARCHIVE_OUTPUT_STAGE_ERROR;
    }
    return result;
}

bool bx_archive_output_file_open(struct bx_archive_output_file* out,
                                 const char* archive_path,
                                 struct bx_diag_ctx* diag) {
    enum bx_archive_output_stage_result stage_result;

    memset(out, 0, sizeof(*out));
    out->display_path = archive_path;

    if (strcmp(archive_path, "-") == 0) {
        out->stream = stdout;
        out->is_stdout = true;
        return true;
    }

    stage_result = bx_archive_output_file_try_stage(out, archive_path, diag);
    if (stage_result == BX_ARCHIVE_OUTPUT_STAGE_READY) {
        return true;
    }
    if (stage_result == BX_ARCHIVE_OUTPUT_STAGE_ERROR) {
        return false;
    }
    return bx_archive_output_file_open_direct(out, archive_path, diag);
}

bool bx_archive_output_file_finish(struct bx_archive_output_file* out,
                                   struct bx_diag_ctx* diag) {
    bool ok = true;
    int fd = -1;

    if (out->stream == NULL) {
        return false;
    }

    if (fflush(out->stream) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
        ok = false;
    }
    if (ok && out->transactional) {
        fd = fileno(out->stream);
        if (fd < 0 || bx_fd_fsync(fd) != 0) {
            bx_diag(diag, "write error: %s", strerror(errno));
            ok = false;
        }
    }
    if (ok && out->transactional && bx_archive_temp_pending_signal() != 0) {
        bx_diag(diag, "interrupted before staged archive publish");
        ok = false;
    }
    if (!out->is_stdout) {
        if (fclose(out->stream) != 0) {
            if (ok) {
                bx_diag(diag, "%s: %s", out->display_path, strerror(errno));
            }
            ok = false;
        }
    }
    out->stream = NULL;
    if (ok && out->transactional && rename(out->temp_path, out->publish_path) != 0) {
        bx_diag(diag, "%s: %s", out->display_path, strerror(errno));
        ok = false;
    }
    if (out->transactional && out->temp_path != NULL) {
        if (!ok) {
            unlink(out->temp_path);
        }
        bx_archive_temp_untrack(out->temp_path);
    }
    free(out->publish_path);
    free(out->temp_path);
    out->publish_path = NULL;
    out->temp_path = NULL;
    return ok;
}

void bx_archive_output_file_discard(struct bx_archive_output_file* out) {
    if (out->stream != NULL && !out->is_stdout) {
        fclose(out->stream);
    }
    if (out->temp_path != NULL) {
        unlink(out->temp_path);
        bx_archive_temp_untrack(out->temp_path);
    }
    free(out->publish_path);
    free(out->temp_path);
    memset(out, 0, sizeof(*out));
}

bool bx_archive_path_has_gzip_suffix(const char* path) {
    size_t len;
    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    return len >= 3u && strcmp(path + len - 3u, ".gz") == 0;
}
