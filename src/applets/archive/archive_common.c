#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "applets/archive/archive_common.h"
#include "bx/libbx.h"
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

bool bx_archive_buffer_read_all(FILE* stream, struct bx_archive_buffer* buffer, struct bx_diag_ctx* diag) {
    unsigned char chunk[8192];

    while (true) {
        size_t nread = fread(chunk, 1u, sizeof(chunk), stream);
        if (nread > 0u && !bx_archive_buffer_append(buffer, chunk, nread)) {
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            return false;
        }
        if (nread < sizeof(chunk)) {
            if (ferror(stream)) {
                bx_diag(diag, "read error: %s", strerror(errno));
                return false;
            }
            return true;
        }
    }
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

bool bx_archive_name_list_read_stream(FILE* stream,
                                      unsigned char separator,
                                      struct bx_archive_name_list* list,
                                      struct bx_diag_ctx* diag) {
    struct bx_archive_buffer input = {0};
    bool ok;

    bx_archive_buffer_init(&input);
    if (!bx_archive_buffer_read_all(stream, &input, diag)) {
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
    FILE* stream;
    bool ok;

    if (strcmp(path, "-") == 0) {
        return bx_archive_name_list_read_stream(stdin, separator, list, diag);
    }

    stream = fopen(path, "rb");
    if (stream == NULL) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }

    ok = bx_archive_name_list_read_stream(stream, separator, list, diag);
    if (fclose(stream) != 0) {
        bx_diag(diag, "%s: %s", path, strerror(errno));
        return false;
    }
    return ok;
}

static bool bx_archive_tempfile_from_buffer(const struct bx_archive_buffer* input, char** path_out, struct bx_diag_ctx* diag) {
    char* path = xstrdup("/tmp/bx-archive-filter-XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        bx_diag(diag, "mkstemp failed: %s", strerror(errno));
        free(path);
        return false;
    }

    if (!bx_xwrite_all(fd, input->data, input->len)) {
        bx_diag(diag, "write error: %s", strerror(errno));
        close(fd);
        unlink(path);
        free(path);
        return false;
    }

    if (close(fd) != 0) {
        bx_diag(diag, "close failed: %s", strerror(errno));
        unlink(path);
        free(path);
        return false;
    }

    *path_out = path;
    return true;
}

bool bx_archive_run_gzip_filter(const struct bx_archive_buffer* input,
                                struct bx_archive_buffer* output,
                                bool decompress,
                                struct bx_diag_ctx* diag) {
    char* tmp_path = NULL;
    int pipefd[2] = {-1, -1};
    pid_t pid;
    int status;

    if (!bx_archive_tempfile_from_buffer(input, &tmp_path, diag)) {
        return false;
    }

    if (pipe(pipefd) != 0) {
        bx_diag(diag, "pipe failed: %s", strerror(errno));
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    pid = fork();
    if (pid < 0) {
        bx_diag(diag, "fork failed: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    if (pid == 0) {
        char* const argv_compress[] = {"gzip", "-c", tmp_path, NULL};
        char* const argv_decompress[] = {"gzip", "-cd", tmp_path, NULL};
        char* const* argv = decompress ? argv_decompress : argv_compress;

        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        close(pipefd[1]);
        execvp("gzip", argv);
        _exit(127);
    }

    close(pipefd[1]);
    while (true) {
        unsigned char chunk[8192];
        ssize_t nread = bx_xread(pipefd[0], chunk, sizeof(chunk));
        if (nread == 0) {
            break;
        }
        if (nread < 0) {
            bx_diag(diag, "read error: %s", strerror(errno));
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            unlink(tmp_path);
            free(tmp_path);
            return false;
        }
        if (!bx_archive_buffer_append(output, chunk, (size_t)nread)) {
            bx_diag(diag, "buffer growth failed: %s", strerror(errno));
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            unlink(tmp_path);
            free(tmp_path);
            return false;
        }
    }
    close(pipefd[0]);

    if (waitpid(pid, &status, 0) < 0) {
        bx_diag(diag, "waitpid failed: %s", strerror(errno));
        unlink(tmp_path);
        free(tmp_path);
        return false;
    }

    unlink(tmp_path);
    free(tmp_path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        bx_diag(diag, "gzip failed");
        return false;
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

    while (offset < len) {
        size_t span = 0u;

        if (sparse && data[offset] == 0u) {
            while (offset + span < len && data[offset + span] == 0u) {
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

        while (offset + span < len && (!sparse || data[offset + span] != 0u)) {
            span++;
        }
        if (!bx_xwrite_all(fd, data + offset, span)) {
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

bool bx_archive_path_has_gzip_suffix(const char* path) {
    size_t len;
    if (path == NULL) {
        return false;
    }
    len = strlen(path);
    return len >= 3u && strcmp(path + len - 3u, ".gz") == 0;
}
