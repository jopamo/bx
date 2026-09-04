#ifndef BX_FETCH_PUBLICATION_H
#define BX_FETCH_PUBLICATION_H

/* BX_FETCH_HEADER_OWNER: core */
/* BX_FETCH_HEADER_CONSUMERS: core */

/*
 * Owns committed in-memory download and URL-map state. A completion is
 * prepared privately and published only when its writer output state proves
 * that the referenced local path is committed or validated unchanged.
 */

#include "config.h"
#include "transfer_completion.h"
#include <stddef.h>
#include <time.h>

typedef struct BxFetchPublicationState BxFetchPublicationState;

typedef enum {
    BX_FETCH_MAPPING_PRIORITY_REDIRECT = 0,
    BX_FETCH_MAPPING_PRIORITY_PERSISTED = 1,
    BX_FETCH_MAPPING_PRIORITY_SIDECAR = 2,
    BX_FETCH_MAPPING_PRIORITY_DIRECT = 3,
} BxFetchMappingPriority;

typedef enum {
    BX_FETCH_PUBLICATION_ERROR = -1,
    BX_FETCH_PUBLICATION_SKIPPED = 0,
    BX_FETCH_PUBLICATION_RECORDED = 1,
} BxFetchPublicationResult;

typedef struct {
    const char* url;
    const char* local_path;
    bool has_server_mtime;
    time_t server_mtime;
} BxFetchDownloadedFileView;

typedef int (*BxFetchPublicationMappingVisitor)(void* userdata, const char* public_url, const char* local_path, BxFetchMappingPriority priority);

BxFetchPublicationState* bx_fetch_publication_state_new(const struct bx_fetch_config* cfg);
void bx_fetch_publication_state_free(BxFetchPublicationState* state);

/*
 * Persistent mappings are used only for link conversion. Loading combines
 * the URL-map store and committed metadata sidecars as one startup
 * transaction: `state` must be pristine, and remains pristine if any
 * recovered entry is invalid or cannot be validated. Saving serializes one
 * immutable snapshot through the transactional URL-map store.
 */
int bx_fetch_publication_load_persisted_mappings(BxFetchPublicationState* state);
int bx_fetch_publication_save_persisted_mappings(const BxFetchPublicationState* state);

BxFetchPublicationResult bx_fetch_publication_record_completion(BxFetchPublicationState* state, const BxFetchTransferCompletion* completion);

/* Boundary lookup removes URL userinfo before consulting the public map. */
const char* bx_fetch_publication_lookup(const BxFetchPublicationState* state, const char* url, BxFetchMappingPriority* priority_out);

size_t bx_fetch_publication_mapping_count(const BxFetchPublicationState* state);
size_t bx_fetch_publication_download_count(const BxFetchPublicationState* state);
bool bx_fetch_publication_latest_download(const BxFetchPublicationState* state, BxFetchDownloadedFileView* view_out);
int bx_fetch_publication_visit_mappings(const BxFetchPublicationState* state, BxFetchPublicationMappingVisitor visitor, void* userdata);

#endif  // BX_FETCH_PUBLICATION_H
