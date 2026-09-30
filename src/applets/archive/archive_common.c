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
        size_t next_cap = list->cap ? list->cap * 2u : 16u;
        next_items = xrealloc(list->items, next_cap * sizeof(*list->items));
        list->items = next_items;
        list->cap = next_cap;
    }

    list->items[list->len++] = xstrdup(name);
    return true;
}

static bool bx_archive_name_list_split_buffer(const struct bx_archive_buffer* input,
                                              unsigned char separator,
                                              struct bx_archive_name_list* list,
                                              struct bx_diag_ctx* diag) {
    size_t start = 0u;
    size_t i;

    for (i = 0u; i <= input->len; i++) {
        bool at_end = (i == input->len);
        bool is_sep = !at_end && input->data[i] == separator;

        if (!at_end && !is_sep) {
            continue;
        }
        if (i > start) {
            size_t item_len = i - start;
            char* item = xmalloc(item_len + 1u);

            memcpy(item, input->data + start, item_len);
            item[item_len] = '\0';
            if (!bx_archive_name_list_append(list, item)) {
                free(item);
                bx_diag(diag, "buffer growth failed: %s", strerror(errno));
                return false;
            }
            free(item);
        }
        start = i + 1u;
    }

    return true;
}

bool bx_archive_name_list_read_fd(int fd, unsigned char separator, struct bx_archive_name_list* list, struct bx_diag_ctx* diag) {
    struct bx_archive_buffer input = {0};
    bool ok;

    bx_archive_buffer_init(&input);
    if (!bx_archive_buffer_read_all(fd, &input, diag)) {
        bx_archive_buffer_free(&input);
        return false;
    }

    ok = bx_archive_name_list_split_buffer(&input, separator, list, diag);
    bx_archive_buffer_free(&input);
    return ok;
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

bool bx_archive_write_regular_payload(int fd,
                                      const unsigned char* data,
                                      size_t len,
                                      bool sparse,
                                      struct bx_diag_ctx* diag) {
    size_t offset = 0u;
    off_t logical_end = 0;
    bool used_sparse = false;

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
            logical_end += (off_t)span;
            offset += span;
            used_sparse = true;
            continue;
        }

        while (span < 65536u && offset + span < len && data[offset + span] != 0u) {
            span++;
        }
        if (!bx_archive_write_payload_bytes(fd, data + offset, span)) {
            bx_diag(diag, "write error: %s", strerror(errno));
            return false;
        }
        logical_end += (off_t)span;
        offset += span;
    }

    if (used_sparse && ftruncate(fd, logical_end) != 0) {
        bx_diag(diag, "write error: %s", strerror(errno));
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
