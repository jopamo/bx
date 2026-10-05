#define _GNU_SOURCE
#include "lib/fetch/client_profile_store.h"
#include "lib/fetch/secure_path.h"
#include "lib/fetch/state_directory.h"
#include "lib/size_parse.h"
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROFILE_ENTRIES 64
#define PROFILE_TTL (7 * 24 * 60 * 60)

typedef struct {
    char origin[1024];
    intmax_t learned;
    BxFetchClientProfile profile;
} ProfileEntry;

struct BxFetchProfileCache {
    ProfileEntry entries[PROFILE_ENTRIES];
};

static bool origin_key(const BxFetchPreparedUrl* target, char key[1024]) {
    BxFetchProtocol protocol = bx_fetch_prepared_url_protocol(target);
    if (protocol != BX_FETCH_PROTOCOL_HTTP && protocol != BX_FETCH_PROTOCOL_HTTPS)
        return false;
    int n = snprintf(key, 1024, "%s://[%s]:%d", bx_fetch_prepared_url_scheme(target), bx_fetch_prepared_url_host(target), bx_fetch_prepared_url_port(target));
    return n > 0 && n < 1024;
}

static bool fresh(const ProfileEntry* entry, intmax_t now) {
    return entry->learned > 0 && entry->learned <= now && now - entry->learned < PROFILE_TTL;
}

static FILE* open_store(bool create) {
    char* directory = bx_fetch_state_directory();
    char* path = NULL;
    char* leaf = NULL;
    if (!directory || asprintf(&path, "%s/client-profiles-v1", directory) < 0) {
        free(directory);
        return NULL;
    }
    free(directory);
    int dirfd = bx_fetch_secure_path_open_parent_directory(path, create, &leaf);
    free(path);
    if (dirfd < 0) {
        free(leaf);
        return NULL;
    }
    int fd = bx_fetch_secure_path_open_leaf(dirfd, leaf, O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (create ? O_RDWR | O_CREAT : O_RDONLY), create ? 0600 : 0);
    close(dirfd);
    free(leaf);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 077) || st.st_size > 70000 ||
        flock(fd, (create ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        close(fd);
        return NULL;
    }
    FILE* file = fdopen(fd, create ? "r+" : "r");
    if (!file)
        close(fd);
    return file;
}

static bool read_store(FILE* file, BxFetchProfileCache* cache) {
    char line[1100];
    size_t count = 0;
    memset(cache, 0, sizeof(*cache));
    while (fgets(line, sizeof(line), file)) {
        if (strcmp(line, "end\n") == 0)
            return fgetc(file) == EOF && !ferror(file);
        if (count == PROFILE_ENTRIES || !strchr(line, '\n'))
            return false;
        ProfileEntry* entry = &cache->entries[count++];
        char* separator = strchr(line, ' ');
        if (!separator || separator[1] < '1' || separator[1] > '3' || separator[2] != ' ')
            return false;
        *separator = '\0';
        uintmax_t learned;
        if (!bx_size_parse_uint(line, &learned) || learned > INTMAX_MAX)
            return false;
        char* origin = separator + 3;
        char* newline = strchr(origin, '\n');
        if (!newline || newline[1] != '\0')
            return false;
        *newline = '\0';
        if (!*origin || strlen(origin) >= sizeof(entry->origin) || strpbrk(origin, " \t\r"))
            return false;
        strcpy(entry->origin, origin);
        entry->learned = (intmax_t)learned;
        entry->profile = (BxFetchClientProfile)(separator[1] - '0');
    }
    return false;
}

BxFetchProfileCache* bx_fetch_profile_cache_new(void) {
    BxFetchProfileCache* cache = calloc(1, sizeof(*cache));
    if (!cache)
        return NULL;
    FILE* file = open_store(false);
    if (file) {
        if (!read_store(file, cache))
            memset(cache, 0, sizeof(*cache));
        fclose(file);
    }
    return cache;
}

void bx_fetch_profile_cache_free(BxFetchProfileCache* cache) {
    free(cache);
}

BxFetchClientProfile bx_fetch_profile_cache_get(BxFetchProfileCache* cache, const BxFetchPreparedUrl* target) {
    char key[1024];
    if (cache && origin_key(target, key)) {
        intmax_t now = time(NULL);
        for (size_t i = 0; i < PROFILE_ENTRIES; i++)
            if (fresh(&cache->entries[i], now) && strcmp(cache->entries[i].origin, key) == 0)
                return cache->entries[i].profile;
    }
    return BX_FETCH_PROFILE_CURL;
}

static void remember(BxFetchProfileCache* cache, const char* key, BxFetchClientProfile profile, intmax_t now) {
    size_t slot = 0;
    for (size_t i = 0; i < PROFILE_ENTRIES; i++) {
        if (strcmp(cache->entries[i].origin, key) == 0) {
            slot = i;
            break;
        }
        if (cache->entries[i].learned < cache->entries[slot].learned)
            slot = i;
    }
    ProfileEntry* entry = &cache->entries[slot];
    strcpy(entry->origin, key);
    entry->profile = profile;
    entry->learned = now;
}

void bx_fetch_profile_cache_put(BxFetchProfileCache* cache, const BxFetchPreparedUrl* target, BxFetchClientProfile profile) {
    char key[1024];
    intmax_t now = time(NULL);
    if (!cache || now <= 0 || profile < BX_FETCH_PROFILE_CURL || profile > BX_FETCH_PROFILE_FIREFOX || !origin_key(target, key))
        return;
    /* Do not extend a cached preference's TTL on every successful request. */
    bool known = false;
    for (size_t i = 0; i < PROFILE_ENTRIES; i++) {
        if (strcmp(cache->entries[i].origin, key) == 0)
            known = true;
        if (fresh(&cache->entries[i], now) && cache->entries[i].profile == profile && strcmp(cache->entries[i].origin, key) == 0)
            return;
    }
    if (!known && profile == BX_FETCH_PROFILE_CURL)
        return;
    remember(cache, key, profile, now);
    BxFetchProfileCache* merged = calloc(1, sizeof(*merged));
    if (!merged)
        return;
    FILE* file = open_store(true);
    if (file) {
        if (!read_store(file, merged))
            memset(merged, 0, sizeof(*merged));
        remember(merged, key, profile, now);
        rewind(file);
        bool ok = ftruncate(fileno(file), 0) == 0;
        for (size_t i = 0; ok && i < PROFILE_ENTRIES; i++) {
            const ProfileEntry* entry = &merged->entries[i];
            if (fresh(entry, now))
                ok = fprintf(file, "%jd %u %s\n", entry->learned, (unsigned)entry->profile, entry->origin) >= 0;
        }
        if (ok)
            fputs("end\n", file);
        fclose(file);
    }
    free(merged);
}
