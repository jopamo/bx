#define _GNU_SOURCE
#include "lib/fetch/publication.h"
#include "lib/fetch/resource_limits.h"
#include "lib/fetch/timestamp_policy.h"
#include "lib/fetch/url.h"
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct MappingNode {
    char* public_url;
    char* local_path;
    BxFetchMappingPriority priority;
    struct MappingNode* next;
} MappingNode;

typedef struct DownloadNode {
    char* url;
    char* local_path;
    bool has_server_mtime;
    time_t server_mtime;
    struct DownloadNode* next;
} DownloadNode;

struct BxFetchPublicationState {
    const struct bx_fetch_config* cfg;
    MappingNode** mapping_buckets;
    size_t mapping_bucket_count;
    size_t mapping_count;
    size_t mapping_bytes;
    DownloadNode* downloads;
    size_t download_count;
    size_t download_bytes;
};

typedef struct {
    MappingNode* existing;
    MappingNode* insertion;
    char* replacement_path;
    BxFetchMappingPriority priority;
    size_t old_path_length;
    size_t new_path_length;
    bool update_priority;
} StagedMapping;

static uint64_t publication_hash(const char* value) {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char* p = (const unsigned char*)value; p && *p; p++) {
        hash ^= *p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static MappingNode* find_public_mapping(const BxFetchPublicationState* state, const char* public_url) {
    if (!state || !public_url || state->mapping_bucket_count == 0)
        return NULL;

    size_t bucket = (size_t)(publication_hash(public_url) % state->mapping_bucket_count);
    for (MappingNode* node = state->mapping_buckets[bucket]; node; node = node->next) {
        if (strcmp(node->public_url, public_url) == 0)
            return node;
    }
    return NULL;
}

static bool should_replace_mapping(const MappingNode* existing, const char* local_path, BxFetchMappingPriority priority) {
    if (!existing || !local_path)
        return false;
    if (priority != existing->priority)
        return priority > existing->priority;
    return strcmp(local_path, existing->local_path) < 0;
}

static void staged_mapping_clear(StagedMapping* staged) {
    if (!staged)
        return;
    if (staged->insertion) {
        free(staged->insertion->public_url);
        free(staged->insertion->local_path);
        free(staged->insertion);
    }
    free(staged->replacement_path);
    *staged = (StagedMapping){0};
}

static int stage_mapping(BxFetchPublicationState* state, const char* public_url, const char* local_path, BxFetchMappingPriority priority, StagedMapping* staged) {
    if (!state || !public_url || !local_path || !staged) {
        errno = EINVAL;
        return -1;
    }

    size_t url_length = 0;
    size_t path_length = 0;
    if (!bx_fetch_resource_bounded_strlen(public_url, BX_FETCH_URL_MAP_MAX_FIELD_BYTES, &url_length) || !bx_fetch_resource_bounded_strlen(local_path, BX_FETCH_URL_MAP_MAX_FIELD_BYTES, &path_length)) {
        errno = EFBIG;
        return -1;
    }

    MappingNode* existing = find_public_mapping(state, public_url);
    staged->existing = existing;
    staged->priority = priority;
    staged->new_path_length = path_length;
    if (existing) {
        staged->old_path_length = strlen(existing->local_path);
        if (!should_replace_mapping(existing, local_path, priority)) {
            staged->update_priority = priority > existing->priority;
            return 0;
        }
        if (strcmp(existing->local_path, local_path) == 0) {
            staged->update_priority = true;
            return 0;
        }
        staged->replacement_path = strdup(local_path);
        return staged->replacement_path ? 0 : -1;
    }

    MappingNode* insertion = calloc(1, sizeof(*insertion));
    if (!insertion)
        return -1;
    insertion->public_url = strdup(public_url);
    insertion->local_path = strdup(local_path);
    insertion->priority = priority;
    if (!insertion->public_url || !insertion->local_path) {
        free(insertion->public_url);
        free(insertion->local_path);
        free(insertion);
        return -1;
    }
    staged->insertion = insertion;
    return 0;
}

static int mapping_reservation(const BxFetchPublicationState* state, const StagedMapping* first, const StagedMapping* second, size_t* final_count, size_t* final_bytes) {
    size_t count = state->mapping_count;
    size_t bytes = state->mapping_bytes;
    const StagedMapping* staged[] = {first, second};
    for (size_t i = 0; i < sizeof(staged) / sizeof(staged[0]); i++) {
        if (!staged[i])
            continue;
        if (staged[i]->insertion) {
            size_t url_length = strlen(staged[i]->insertion->public_url);
            if (url_length > SIZE_MAX - staged[i]->new_path_length ||
                !bx_fetch_resource_can_reserve(count, bytes, 1, url_length + staged[i]->new_path_length, BX_FETCH_URL_MAP_MAX_ENTRIES, BX_FETCH_URL_MAP_MAX_DECODED_BYTES)) {
                errno = EFBIG;
                return -1;
            }
            count++;
            bytes += url_length + staged[i]->new_path_length;
        }
        else if (staged[i]->replacement_path) {
            if (staged[i]->old_path_length > bytes) {
                errno = EINVAL;
                return -1;
            }
            size_t retained = bytes - staged[i]->old_path_length;
            if (!bx_fetch_resource_can_reserve(count, retained, 0, staged[i]->new_path_length, BX_FETCH_URL_MAP_MAX_ENTRIES, BX_FETCH_URL_MAP_MAX_DECODED_BYTES)) {
                errno = EFBIG;
                return -1;
            }
            bytes = retained + staged[i]->new_path_length;
        }
    }
    *final_count = count;
    *final_bytes = bytes;
    return 0;
}

static int ensure_mapping_capacity(BxFetchPublicationState* state, size_t final_count) {
    size_t desired = state->mapping_bucket_count ? state->mapping_bucket_count : 16;
    while (final_count > desired - desired / 4) {
        if (desired > SIZE_MAX / 2) {
            errno = ENOMEM;
            return -1;
        }
        desired *= 2;
    }
    if (desired == state->mapping_bucket_count)
        return 0;
    if (desired > SIZE_MAX / sizeof(*state->mapping_buckets)) {
        errno = ENOMEM;
        return -1;
    }

    MappingNode** buckets = calloc(desired, sizeof(*buckets));
    if (!buckets)
        return -1;
    for (size_t i = 0; i < state->mapping_bucket_count; i++) {
        MappingNode* node = state->mapping_buckets[i];
        while (node) {
            MappingNode* next = node->next;
            size_t bucket = (size_t)(publication_hash(node->public_url) % desired);
            node->next = buckets[bucket];
            buckets[bucket] = node;
            node = next;
        }
    }
    free(state->mapping_buckets);
    state->mapping_buckets = buckets;
    state->mapping_bucket_count = desired;
    return 0;
}

static void commit_staged_mapping(BxFetchPublicationState* state, StagedMapping* staged) {
    if (staged->insertion) {
        size_t bucket = (size_t)(publication_hash(staged->insertion->public_url) % state->mapping_bucket_count);
        staged->insertion->next = state->mapping_buckets[bucket];
        state->mapping_buckets[bucket] = staged->insertion;
        staged->insertion = NULL;
        return;
    }
    if (staged->replacement_path) {
        free(staged->existing->local_path);
        staged->existing->local_path = staged->replacement_path;
        staged->replacement_path = NULL;
        staged->existing->priority = staged->priority;
    }
    else if (staged->update_priority) {
        staged->existing->priority = staged->priority;
    }
}

static bool completion_can_publish(const BxFetchPublicationState* state, const BxFetchTransferCompletion* completion) {
    if (!state || !completion || !completion->request || !completion->response || !completion->output_path)
        return false;
    if (completion->result != BX_FETCH_OK || state->cfg->download.spider || strcmp(completion->output_path, "-") == 0)
        return false;
    if (completion->response->status_code != 200 && completion->response->status_code != 206 && completion->response->status_code != 304)
        return false;
    return completion->response->output_state == BX_FETCH_OUTPUT_STATE_COMMITTED || completion->response->output_state == BX_FETCH_OUTPUT_STATE_METADATA_COMMITTED ||
           completion->response->output_state == BX_FETCH_OUTPUT_STATE_UNCHANGED;
}

BxFetchPublicationState* bx_fetch_publication_state_new(const struct bx_fetch_config* cfg) {
    if (!cfg) {
        errno = EINVAL;
        return NULL;
    }
    BxFetchPublicationState* state = calloc(1, sizeof(*state));
    if (!state)
        return NULL;
    state->cfg = cfg;
    return state;
}

void bx_fetch_publication_state_free(BxFetchPublicationState* state) {
    if (!state)
        return;
    for (size_t i = 0; i < state->mapping_bucket_count; i++) {
        MappingNode* node = state->mapping_buckets[i];
        while (node) {
            MappingNode* next = node->next;
            free(node->public_url);
            free(node->local_path);
            free(node);
            node = next;
        }
    }
    DownloadNode* download = state->downloads;
    while (download) {
        DownloadNode* next = download->next;
        free(download->url);
        free(download->local_path);
        free(download);
        download = next;
    }
    free(state->mapping_buckets);
    free(state);
}

BxFetchPublicationResult bx_fetch_publication_record_completion(BxFetchPublicationState* state, const BxFetchTransferCompletion* completion) {
    if (!state || !completion || !completion->request || !completion->response || !completion->output_path) {
        errno = EINVAL;
        return BX_FETCH_PUBLICATION_ERROR;
    }
    if (!completion_can_publish(state, completion))
        return BX_FETCH_PUBLICATION_SKIPPED;

    const char* request_url = bx_fetch_request_url_for_display(completion->request);
    const BxFetchPreparedUrl* effective_target = bx_fetch_response_effective_target(completion->response);
    const char* effective_url = effective_target ? bx_fetch_prepared_url_display(effective_target) : request_url;
    if (!request_url || !effective_url) {
        errno = EINVAL;
        return BX_FETCH_PUBLICATION_ERROR;
    }

    size_t download_url_length = 0;
    size_t path_length = 0;
    if (!bx_fetch_resource_bounded_strlen(effective_url, BX_FETCH_URL_MAX_BYTES, &download_url_length) ||
        !bx_fetch_resource_bounded_strlen(completion->output_path, BX_FETCH_URL_MAP_MAX_FIELD_BYTES, &path_length) || download_url_length > SIZE_MAX - path_length ||
        !bx_fetch_resource_can_reserve(state->download_count, state->download_bytes, 1, download_url_length + path_length, BX_FETCH_URL_STATE_MAX_ENTRIES, BX_FETCH_URL_STATE_MAX_BYTES)) {
        errno = EFBIG;
        return BX_FETCH_PUBLICATION_ERROR;
    }

    DownloadNode* download = calloc(1, sizeof(*download));
    if (!download)
        return BX_FETCH_PUBLICATION_ERROR;
    download->url = strdup(effective_url);
    download->local_path = strdup(completion->output_path);
    if (!download->url || !download->local_path) {
        free(download->url);
        free(download->local_path);
        free(download);
        return BX_FETCH_PUBLICATION_ERROR;
    }
    const char* last_modified = bx_fetch_response_header_value(completion->response, "Last-Modified");
    download->has_server_mtime =
        bx_fetch_timestamp_should_use_server_time(state->cfg->download.no_use_server_timestamps, completion->response->status_code, completion->output_path, last_modified, &download->server_mtime);

    StagedMapping direct = {0};
    StagedMapping redirect = {0};
    if (stage_mapping(state, request_url, completion->output_path, BX_FETCH_MAPPING_PRIORITY_DIRECT, &direct) != 0 ||
        (strcmp(request_url, effective_url) != 0 && stage_mapping(state, effective_url, completion->output_path, BX_FETCH_MAPPING_PRIORITY_REDIRECT, &redirect) != 0)) {
        staged_mapping_clear(&direct);
        staged_mapping_clear(&redirect);
        free(download->url);
        free(download->local_path);
        free(download);
        return BX_FETCH_PUBLICATION_ERROR;
    }

    size_t final_mapping_count = 0;
    size_t final_mapping_bytes = 0;
    if (mapping_reservation(state, &direct, strcmp(request_url, effective_url) != 0 ? &redirect : NULL, &final_mapping_count, &final_mapping_bytes) != 0 ||
        ensure_mapping_capacity(state, final_mapping_count) != 0) {
        staged_mapping_clear(&direct);
        staged_mapping_clear(&redirect);
        free(download->url);
        free(download->local_path);
        free(download);
        return BX_FETCH_PUBLICATION_ERROR;
    }

    commit_staged_mapping(state, &direct);
    if (strcmp(request_url, effective_url) != 0)
        commit_staged_mapping(state, &redirect);
    state->mapping_count = final_mapping_count;
    state->mapping_bytes = final_mapping_bytes;

    download->next = state->downloads;
    state->downloads = download;
    state->download_count++;
    state->download_bytes += download_url_length + path_length;
    return BX_FETCH_PUBLICATION_RECORDED;
}

const char* bx_fetch_publication_lookup(const BxFetchPublicationState* state, const char* url, BxFetchMappingPriority* priority_out) {
    if (!state || !url) {
        errno = EINVAL;
        return NULL;
    }
    char* public_url = bx_fetch_url_display_safe(url);
    if (!public_url)
        return NULL;
    MappingNode* mapping = find_public_mapping(state, public_url);
    free(public_url);
    if (!mapping)
        return NULL;
    if (priority_out)
        *priority_out = mapping->priority;
    return mapping->local_path;
}

size_t bx_fetch_publication_mapping_count(const BxFetchPublicationState* state) {
    return state ? state->mapping_count : 0;
}

size_t bx_fetch_publication_download_count(const BxFetchPublicationState* state) {
    return state ? state->download_count : 0;
}

bool bx_fetch_publication_latest_download(const BxFetchPublicationState* state, BxFetchDownloadedFileView* view_out) {
    if (!state || !view_out || !state->downloads)
        return false;
    *view_out = (BxFetchDownloadedFileView){
        .url = state->downloads->url,
        .local_path = state->downloads->local_path,
        .has_server_mtime = state->downloads->has_server_mtime,
        .server_mtime = state->downloads->server_mtime,
    };
    return true;
}

int bx_fetch_publication_visit_mappings(const BxFetchPublicationState* state, BxFetchPublicationMappingVisitor visitor, void* userdata) {
    if (!state || !visitor) {
        errno = EINVAL;
        return -1;
    }
    for (size_t i = 0; i < state->mapping_bucket_count; i++) {
        for (MappingNode* node = state->mapping_buckets[i]; node; node = node->next) {
            if (visitor(userdata, node->public_url, node->local_path, node->priority) != 0)
                return -1;
        }
    }
    return 0;
}
