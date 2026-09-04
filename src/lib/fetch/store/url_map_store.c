#define _GNU_SOURCE
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/secure_path.h"
#include "lib/fetch/url_map_store.h"
#include "lib/fetch/url.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define URL_MAP_STORE_FILE_PREFIX "url-map-"

typedef struct {
    char* url;
    const char* local_path;
} EntryRef;

static void entry_refs_free(EntryRef* entries, size_t entry_count) {
    if (!entries)
        return;
    for (size_t i = 0; i < entry_count; i++) {
        free(entries[i].url);
    }
    free(entries);
}

static int url_map_store_fail_errno(int error_number) {
    errno = error_number;
    return -1;
}

static int url_map_store_fail_tmp_file(int error_number, int dirfd, int fd, char** tmp_name_inout) {
    if (fd != -1)
        close(fd);
    if (dirfd != -1 && tmp_name_inout && *tmp_name_inout) {
        unlinkat(dirfd, *tmp_name_inout, 0);
        free(*tmp_name_inout);
        *tmp_name_inout = NULL;
    }
    return url_map_store_fail_errno(error_number);
}

static char* output_document_dir(const EffectiveConfig* cfg) {
    if (!cfg || !cfg->download.output_document || cfg->download.output_document[0] == '\0') {
        return NULL;
    }

    const char* output_document = cfg->download.output_document;
    const char* last_slash = strrchr(output_document, '/');
    if (!last_slash)
        return NULL;
    if (last_slash == output_document)
        return strdup("/");

    return strndup(output_document, (size_t)(last_slash - output_document));
}

static char* store_scope_for(const EffectiveConfig* cfg) {
    if (!cfg)
        return NULL;

    if (cfg->dirs.directory_prefix && cfg->dirs.directory_prefix[0] != '\0') {
        return strdup(cfg->dirs.directory_prefix);
    }

    char* output_dir = output_document_dir(cfg);
    if (output_dir)
        return output_dir;

    char* cwd = getcwd(NULL, 0);
    if (cwd)
        return cwd;

    return strdup("/");
}

static uint64_t fnv1a64(const char* value) {
    uint64_t hash = 1469598103934665603ULL;
    if (!value)
        return hash;

    for (const unsigned char* p = (const unsigned char*)value; *p != '\0'; p++) {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }

    return hash;
}

static char* state_dir_for_store(void) {
    const char* xdg_state_home = getenv("XDG_STATE_HOME");
    if (xdg_state_home && xdg_state_home[0] != '\0') {
        char* path = NULL;
        if (asprintf(&path, "%s/mira", xdg_state_home) == -1) {
            return NULL;
        }
        return path;
    }

    const char* home = getenv("HOME");
    if (home && home[0] != '\0') {
        char* path = NULL;
        if (asprintf(&path, "%s/.local/state/mira", home) == -1) {
            return NULL;
        }
        return path;
    }

    char* path = NULL;
    if (asprintf(&path, "/tmp/mira-%lu", (unsigned long)getuid()) == -1) {
        return NULL;
    }
    return path;
}

static char* store_path_for(const EffectiveConfig* cfg) {
    if (!cfg)
        return NULL;

    char* store_dir = state_dir_for_store();
    if (!store_dir)
        return NULL;

    char* scope = store_scope_for(cfg);
    if (!scope) {
        free(store_dir);
        return NULL;
    }

    uint64_t scope_hash = fnv1a64(scope);
    free(scope);

    char* path = NULL;
    if (asprintf(&path, "%s/%s%016llx", store_dir, URL_MAP_STORE_FILE_PREFIX, (unsigned long long)scope_hash) == -1) {
        free(store_dir);
        return NULL;
    }

    free(store_dir);
    return path;
}

static char hex_from_nibble(unsigned char value) {
    return (value < 10U) ? (char)('0' + value) : (char)('a' + (value - 10U));
}

static int nibble_from_hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

static int hex_encode(const char* input, char** encoded_out) {
    if (!input || !encoded_out)
        return -1;

    size_t len = strlen(input);
    if (len > (SIZE_MAX - 1U) / 2U)
        return -1;

    char* encoded = malloc((len * 2U) + 1U);
    if (!encoded)
        return -1;

    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)input[i];
        encoded[i * 2U] = hex_from_nibble((unsigned char)((b >> 4U) & 0x0FU));
        encoded[(i * 2U) + 1U] = hex_from_nibble((unsigned char)(b & 0x0FU));
    }
    encoded[len * 2U] = '\0';

    *encoded_out = encoded;
    return 0;
}

static int hex_decode(const char* encoded, char** decoded_out) {
    if (!encoded || !decoded_out) {
        errno = EINVAL;
        return -1;
    }

    size_t len = strlen(encoded);
    if ((len % 2U) != 0U || len / 2u > MIRA_URL_MAP_MAX_FIELD_BYTES) {
        errno = len / 2u > MIRA_URL_MAP_MAX_FIELD_BYTES ? EFBIG : EINVAL;
        return -1;
    }

    char* decoded = malloc((len / 2U) + 1U);
    if (!decoded)
        return -1;

    for (size_t i = 0; i < len; i += 2U) {
        int hi = nibble_from_hex(encoded[i]);
        int lo = nibble_from_hex(encoded[i + 1U]);
        if (hi < 0 || lo < 0) {
            free(decoded);
            errno = EINVAL;
            return -1;
        }
        unsigned char byte = (unsigned char)((hi << 4) | lo);
        if (byte == '\0') {
            free(decoded);
            errno = EINVAL;
            return -1;
        }
        decoded[i / 2U] = (char)byte;
    }
    decoded[len / 2U] = '\0';

    *decoded_out = decoded;
    return 0;
}

static int entry_ref_cmp(const void* a, const void* b) {
    const EntryRef* lhs = a;
    const EntryRef* rhs = b;

    int url_cmp = strcmp(lhs->url, rhs->url);
    if (url_cmp != 0)
        return url_cmp;
    return strcmp(lhs->local_path, rhs->local_path);
}

static int open_store_dir(const char* path, int* dirfd_out, char** basename_out) {
    if (!path || !dirfd_out || !basename_out) {
        errno = EINVAL;
        return -1;
    }

    char* basename = NULL;
    int dirfd = mira_secure_path_open_parent_directory(path, true, &basename);
    if (dirfd == -1)
        return -1;

    *dirfd_out = dirfd;
    *basename_out = basename;
    return 0;
}

static bool owner_pid_is_live(pid_t pid) {
    if (pid <= 0)
        return false;

    if (kill(pid, 0) == 0)
        return true;
    return errno == EPERM;
}

static int cleanup_orphan_temp_files(int dirfd, const char* basename, bool* removed_any) {
    if (dirfd == -1 || !basename) {
        errno = EINVAL;
        return -1;
    }

    if (removed_any)
        *removed_any = false;

    char* prefix = NULL;
    if (asprintf(&prefix, "%s.tmp.", basename) == -1) {
        return -1;
    }

    int scan_fd = dup(dirfd);
    if (scan_fd == -1) {
        free(prefix);
        return -1;
    }

    DIR* dir = fdopendir(scan_fd);
    if (!dir) {
        free(prefix);
        close(scan_fd);
        return -1;
    }

    size_t prefix_len = strlen(prefix);
    int rc = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, prefix, prefix_len) != 0) {
            continue;
        }

        const char* pid_text = ent->d_name + prefix_len;
        if (pid_text[0] == '\0')
            continue;

        errno = 0;
        char* end = NULL;
        long parsed = strtol(pid_text, &end, 10);
        if (errno != 0 || !end || end == pid_text || *end != '.' || parsed <= 0) {
            continue;
        }

        if (owner_pid_is_live((pid_t)parsed)) {
            continue;
        }

        if (unlinkat(dirfd, ent->d_name, 0) != 0 && errno != ENOENT) {
            rc = -1;
            break;
        }

        if (removed_any)
            *removed_any = true;
    }

    int closed_rc = closedir(dir);
    free(prefix);
    if (rc != 0)
        return -1;
    if (closed_rc != 0)
        return -1;
    return 0;
}

static FILE* open_unique_tmp_file(int dirfd, const char* basename, char** tmp_name_out) {
    if (dirfd == -1 || !basename || !tmp_name_out)
        return NULL;

    static _Atomic unsigned long long counter = 0;
    for (unsigned int attempt = 0; attempt < 128; attempt++) {
        unsigned long long sequence = atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
        char* tmp_name = NULL;
        if (asprintf(&tmp_name, "%s.tmp.%ld.%llu.%u", basename, (long)getpid(), sequence, attempt) == -1) {
            return NULL;
        }
        int fd = openat(dirfd, tmp_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd == -1) {
            int error_number = errno;
            free(tmp_name);
            if (error_number == EEXIST)
                continue;
            errno = error_number;
            return NULL;
        }
        FILE* f = fdopen(fd, "w");
        if (!f) {
            url_map_store_fail_tmp_file(errno, dirfd, fd, &tmp_name);
            return NULL;
        }
        *tmp_name_out = tmp_name;
        return f;
    }
    errno = EEXIST;
    return NULL;
}

static int flush_and_sync_stream(FILE* f) {
    if (!f) {
        errno = EINVAL;
        return -1;
    }

    if (fflush(f) != 0) {
        return -1;
    }

    int fd = fileno(f);
    if (fd == -1) {
        return -1;
    }

    return fsync(fd);
}

typedef enum {
    STORE_LINE_OK = 0,
    STORE_LINE_EOF,
    STORE_LINE_ERROR,
} StoreLineResult;

/*
 * The store is attacker-modifiable persistent input. Read it incrementally so
 * neither a growing file nor a single missing-newline record can make getline
 * allocate beyond the store contract.
 */
static StoreLineResult read_store_line(FILE* f, char* line, size_t capacity, size_t* store_bytes) {
    if (!f || !line || !store_bytes || capacity < MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES + 1u) {
        errno = EINVAL;
        return STORE_LINE_ERROR;
    }

    size_t length = 0;
    for (;;) {
        int byte = fgetc(f);
        if (byte == EOF) {
            if (ferror(f)) {
                if (!errno)
                    errno = EIO;
                return STORE_LINE_ERROR;
            }
            if (length == 0)
                return STORE_LINE_EOF;
            break;
        }

        if (*store_bytes >= MIRA_URL_MAP_STORE_MAX_BYTES || length >= MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES) {
            errno = EFBIG;
            return STORE_LINE_ERROR;
        }
        (*store_bytes)++;

        if (byte == '\0') {
            errno = EINVAL;
            return STORE_LINE_ERROR;
        }
        line[length++] = (char)byte;
        if (byte == '\n')
            break;
    }

    line[length] = '\0';
    return STORE_LINE_OK;
}

int url_map_store_load(const EffectiveConfig* cfg, MiraUrlMapLoadFn cb, void* userdata) {
    if (!cfg || !cb) {
        errno = EINVAL;
        return -1;
    }

    char* path = store_path_for(cfg);
    if (!path)
        return -1;

    int dirfd = -1;
    char* basename = NULL;
    if (open_store_dir(path, &dirfd, &basename) != 0) {
        free(path);
        return -1;
    }

    bool removed_orphans = false;
    if (cleanup_orphan_temp_files(dirfd, basename, &removed_orphans) != 0) {
        close(dirfd);
        free(basename);
        free(path);
        return -1;
    }
    if (removed_orphans && fsync(dirfd) != 0) {
        close(dirfd);
        free(basename);
        free(path);
        return -1;
    }

    int store_fd = mira_secure_path_open_leaf(dirfd, basename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
    int open_error_number = errno;
    close(dirfd);
    free(basename);
    free(path);
    if (store_fd == -1) {
        errno = open_error_number;
        return (open_error_number == ENOENT) ? 0 : -1;
    }
    struct stat store_stat;
    if (fstat(store_fd, &store_stat) != 0) {
        int error_number = errno;
        close(store_fd);
        errno = error_number;
        return -1;
    }
    /*
     * A concurrent atomic replacement can unlink this already-open snapshot,
     * producing nlink == 0 while the descriptor remains the pinned read
     * authority. Only multiple names create the hard-link ambiguity.
     */
    if (!S_ISREG(store_stat.st_mode) || store_stat.st_nlink > 1) {
        close(store_fd);
        errno = EINVAL;
        return -1;
    }
    FILE* f = fdopen(store_fd, "r");
    if (!f) {
        int error_number = errno;
        close(store_fd);
        errno = error_number;
        return -1;
    }

    int rc = 0;
    int failure_errno = 0;
    char* line = malloc(MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES + 1u);
    if (!line) {
        failure_errno = errno ? errno : ENOMEM;
        rc = -1;
    }
    size_t store_bytes = 0;
    size_t entry_count = 0;
    size_t decoded_bytes = 0;
    while (true) {
        if (rc != 0)
            break;
        StoreLineResult line_result = read_store_line(f, line, MIRA_URL_MAP_ENCODED_LINE_MAX_BYTES + 1u, &store_bytes);
        if (line_result == STORE_LINE_EOF)
            break;
        if (line_result == STORE_LINE_ERROR) {
            failure_errno = errno ? errno : EIO;
            rc = -1;
            break;
        }

        size_t line_len = strlen(line);
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r')) {
            line[--line_len] = '\0';
        }
        if (line_len == 0)
            continue;

        char* tab = strchr(line, '\t');
        if (!tab || tab == line || tab[1] == '\0' || strchr(tab + 1, '\t')) {
            failure_errno = EINVAL;
            rc = -1;
            break;
        }

        *tab = '\0';
        const char* url_hex = line;
        const char* path_hex = tab + 1;

        char* url = NULL;
        char* local_path = NULL;
        if (hex_decode(url_hex, &url) != 0 || hex_decode(path_hex, &local_path) != 0) {
            failure_errno = errno ? errno : EINVAL;
            free(url);
            free(local_path);
            rc = -1;
            break;
        }

        size_t url_len = strlen(url);
        size_t path_len = strlen(local_path);
        if (url_len > SIZE_MAX - path_len || !mira_resource_can_reserve(entry_count, decoded_bytes, 1u, url_len + path_len, MIRA_URL_MAP_MAX_ENTRIES, MIRA_URL_MAP_MAX_DECODED_BYTES)) {
            free(url);
            free(local_path);
            failure_errno = EFBIG;
            rc = -1;
            break;
        }

        if (cb(userdata, url, local_path) != 0) {
            failure_errno = errno ? errno : EINVAL;
            free(url);
            free(local_path);
            rc = -1;
            break;
        }

        entry_count++;
        decoded_bytes += url_len + path_len;
        free(url);
        free(local_path);
    }

    free(line);
    if (fclose(f) != 0 && rc == 0) {
        failure_errno = errno ? errno : EIO;
        rc = -1;
    }
    if (rc != 0)
        errno = failure_errno ? failure_errno : EIO;
    return rc;
}

int url_map_store_save(const EffectiveConfig* cfg, const MiraUrlMapEntry* entries, size_t entry_count) {
    if (!cfg) {
        errno = EINVAL;
        return -1;
    }
    if (entry_count > MIRA_URL_MAP_MAX_ENTRIES) {
        errno = EFBIG;
        return -1;
    }
    if (entry_count > 0 && !entries) {
        errno = EINVAL;
        return -1;
    }

    EntryRef* sorted = NULL;
    if (entry_count > 0) {
        sorted = calloc(entry_count, sizeof(EntryRef));
        if (!sorted)
            return -1;
    }

    size_t decoded_bytes = 0;
    for (size_t i = 0; i < entry_count; i++) {
        size_t source_url_len = 0;
        size_t path_len = 0;
        if (!entries[i].url || !entries[i].local_path) {
            entry_refs_free(sorted, entry_count);
            errno = EINVAL;
            return -1;
        }
        if (!mira_resource_bounded_strlen(entries[i].url, MIRA_URL_MAP_MAX_FIELD_BYTES, &source_url_len) ||
            !mira_resource_bounded_strlen(entries[i].local_path, MIRA_URL_MAP_MAX_FIELD_BYTES, &path_len)) {
            entry_refs_free(sorted, entry_count);
            errno = EFBIG;
            return -1;
        }

        sorted[i].url = mira_url_display_safe(entries[i].url);
        if (!sorted[i].url) {
            entry_refs_free(sorted, entry_count);
            if (!errno)
                errno = EINVAL;
            return -1;
        }
        size_t public_url_len = 0;
        if (!mira_resource_bounded_strlen(sorted[i].url, MIRA_URL_MAP_MAX_FIELD_BYTES, &public_url_len) || public_url_len > SIZE_MAX - path_len ||
            !mira_resource_can_reserve(i, decoded_bytes, 1u, public_url_len + path_len, MIRA_URL_MAP_MAX_ENTRIES, MIRA_URL_MAP_MAX_DECODED_BYTES)) {
            entry_refs_free(sorted, entry_count);
            errno = EFBIG;
            return -1;
        }
        decoded_bytes += public_url_len + path_len;
        sorted[i].local_path = entries[i].local_path;
    }
    if (entry_count > 0) {
        qsort(sorted, entry_count, sizeof(EntryRef), entry_ref_cmp);
    }

    char* path = store_path_for(cfg);
    if (!path) {
        entry_refs_free(sorted, entry_count);
        return -1;
    }

    int dirfd = -1;
    char* basename = NULL;
    if (open_store_dir(path, &dirfd, &basename) != 0) {
        entry_refs_free(sorted, entry_count);
        free(path);
        return -1;
    }

    if (cleanup_orphan_temp_files(dirfd, basename, NULL) != 0) {
        entry_refs_free(sorted, entry_count);
        close(dirfd);
        free(basename);
        free(path);
        return -1;
    }

    if (entry_count == 0) {
        int unlink_rc = unlinkat(dirfd, basename, 0);
        if (unlink_rc != 0 && errno != ENOENT) {
            close(dirfd);
            free(basename);
            free(path);
            return -1;
        }
        if (fsync(dirfd) != 0) {
            close(dirfd);
            free(basename);
            free(path);
            return -1;
        }
        close(dirfd);
        free(basename);
        free(path);
        return 0;
    }

    char* tmp_name = NULL;
    FILE* f = open_unique_tmp_file(dirfd, basename, &tmp_name);
    if (!f) {
        free(tmp_name);
        entry_refs_free(sorted, entry_count);
        close(dirfd);
        free(basename);
        free(path);
        return -1;
    }

    int rc = 0;
    for (size_t i = 0; i < entry_count; i++) {
        char* url_hex = NULL;
        char* path_hex = NULL;
        if (hex_encode(sorted[i].url, &url_hex) != 0 || hex_encode(sorted[i].local_path, &path_hex) != 0) {
            free(url_hex);
            free(path_hex);
            rc = -1;
            break;
        }

        if (fprintf(f, "%s\t%s\n", url_hex, path_hex) < 0) {
            rc = -1;
            free(url_hex);
            free(path_hex);
            break;
        }

        free(url_hex);
        free(path_hex);
    }

    if (rc == 0 && flush_and_sync_stream(f) != 0)
        rc = -1;
    if (fclose(f) != 0)
        rc = -1;

    if (rc == 0 && renameat(dirfd, tmp_name, dirfd, basename) != 0) {
        rc = -1;
    }
    if (rc == 0 && fsync(dirfd) != 0)
        rc = -1;
    if (rc != 0)
        unlinkat(dirfd, tmp_name, 0);

    free(tmp_name);
    entry_refs_free(sorted, entry_count);
    close(dirfd);
    free(basename);
    free(path);
    return rc;
}
